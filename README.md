# Terraria 3D (v0.5.5)

A Luma3DS 3GX plugin that adds real per-layer stereoscopic 3D to Terraria on the
Nintendo 3DS (New 3DS / New 2DS XL only).

## Features

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

- New Nintendo 3DS / New 2DS XL (Old 3DS is not supported) with Luma3DS v13.0 or newer
- Terraria, USA version (`000400000016A900`), updated to v5.6.0

## Quick install

1. Copy `Terraria3D.3gx` to `SD:/luma/plugins/000400000016A900/Terraria3D.3gx`.
2. In the Rosalina menu (L + D-pad Down + SELECT), set "Plugin loader" to Enabled.
3. Launch Terraria, then press SELECT for the mod menu.

Full instructions, region notes and troubleshooting: see [INSTALL.md](INSTALL.md).

## Legal

This repository contains no game files; you need your own legally owned copy of
Terraria. Terraria 3D is a fan mod, not affiliated with Re-Logic, 505 Games or Nintendo.

Built with CTRPluginFramework, Luma3DS and devkitPro.

## Credits

Developed by Claude (Anthropic) as an AI-driven reverse-engineering project, with mattvideoproductions directing and testing on hardware. Uses CTRPluginFramework and Luma3DS's plugin loader.

Not affiliated with Re-Logic, 505 Games or Nintendo. No game files are included; you need your own copy.
