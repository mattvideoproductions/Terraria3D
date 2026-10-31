# Terraria 3D (v0.5.6 New 3DS, v0.7.0 Old 3DS)

A Luma3DS 3GX plugin that adds real per-layer stereoscopic 3D to Terraria on the
Nintendo 3DS. There are two builds: one for the New 3DS and one for the Old 3DS.

> **A MattVidPro project.** Made for the [MattVidPro YouTube channel](https://www.youtube.com/@MattVidPro): Claude AI built it, and Matt directed and tested it on a real New 3DS and Old 3DS.

**In plain words:** this mod makes Terraria on your 3DS pop out in real 3D. The sky, the background walls, the blocks and the HUD each sit at their own depth, like looking into a little terrarium. It also adds better controls and smoother play. If your 3DS already runs custom firmware (Luma3DS), installing it means copying one file to your SD card. Check which file you need below, download it, then follow the install steps.

## Which file do I need?

| Your console | Download | Install guide |
|---|---|---|
| **New 3DS**, New 3DS XL, New 2DS XL | **[Terraria 3D v0.5.6](https://github.com/mattvideoproductions/Terraria3D/releases/latest)** (`Terraria3D.3gx`) | [INSTALL.md](INSTALL.md) |
| **Old 3DS**, 3DS XL, 2DS (the originals) | **[Terraria 3D v0.7.0 for Old 3DS](https://github.com/mattvideoproductions/Terraria3D/releases/tag/o3ds-v0.7.0)** (`Terraria3D-Old3DS-v0.7.0.zip`) | [INSTALL_OLD_3DS.md](INSTALL_OLD_3DS.md) |

Not sure which you have? A New 3DS has a small C-stick nub above the ABXY buttons and ZL / ZR buttons on the back. If yours doesn't, it's an Old 3DS. Use only one of the two files: both go in the same folder with the same name.

The Old 3DS build has real layered 3D too, in a lighter form (4 depth layers), plus a 30 fps lock to keep it smooth. It uses the game's own controls, since the Old 3DS has no C-stick or ZL / ZR.

## Features (New 3DS build)

- Per-layer stereo 3D (sky, walls, tiles, particles and HUD at different depths)
- Depth presets, default "Terrarium Window"
- 2x zoom (L + R)
- C-stick aiming (see Controls)
- Cheat console and Creative mode menu
- Optional fake-OLED colour booster
- Tap to move items (Vita style) in the inventory and chests, including across tabs
- "Held item hint" toggle: the on-screen note shown while an item is carried to another tab (on by default)

## Controls

- **ZR**: use / mine / place at the C-stick aim point
- **ZL**: switch the C-stick style (Spring / Cursor); a short notice names the style
- **A**: interact and grappling hook (stock). "ZL = grapple" is still in the menu
  (ZL button, or the "New 3DS, ZL grapple" preset)
- **Y**: stock behaviour (smart-cursor stock/break when the C-stick is centred)
- **C-stick**: aim; with a pickaxe, axe or hammer, a half push auto-tunnels in that
  direction ("C-stick tool mode": Auto-tunnel or Precise)
- **D-pad**: stock cursor, or walking if "D-pad walking" is on (Controls menu)
- **Inventory and chests**: tap to move items (tap a stack, then tap a slot to place, swap
  or merge; works across the chest / inventory tabs)
- **L + R**: 2x zoom; **SELECT**: mod menu; **L + R + Y**: cheat console

## Requirements

- Any 3DS or 2DS with Luma3DS v13.0 or newer (pick the right build above)
- Terraria, USA version (`000400000016A900`), updated to v5.6.0

## Quick install (New 3DS)

1. Copy `Terraria3D.3gx` to `SD:/luma/plugins/000400000016A900/Terraria3D.3gx`.
2. In the Rosalina menu (L + D-pad Down + SELECT), set "Plugin loader" to Enabled.
3. Launch Terraria, then press SELECT for the mod menu.

Full instructions, region notes and troubleshooting: see [INSTALL.md](INSTALL.md). Old 3DS: see [INSTALL_OLD_3DS.md](INSTALL_OLD_3DS.md).

## Legal

This repository contains no game files; you need your own legally owned copy of
Terraria. Terraria 3D is a fan mod, not affiliated with Re-Logic, 505 Games or Nintendo.

Built with CTRPluginFramework, Luma3DS and devkitPro.

## Credits

Developed by Claude (Anthropic) as an AI-driven reverse-engineering project, with Matt ([MattVidPro](https://www.youtube.com/@MattVidPro)) directing and testing on hardware. Uses CTRPluginFramework and Luma3DS's plugin loader.

Not affiliated with Re-Logic, 505 Games or Nintendo. No game files are included; you need your own copy.
