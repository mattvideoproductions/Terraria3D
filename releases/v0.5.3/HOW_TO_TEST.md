# Terraria 3D mod v0.5.3: test plan (New 3DS only)

File: `sd/luma/plugins/000400000016A900/Terraria3D.3gx`. The perf log stays on; the log now also records every inventory tap ("inv tap:" lines). Benchmark with Fullbright OFF. This build was audited but NOT emulator-tested (you were recording).

## Changes
- **OLED booster no longer rewrites the screen colour table every second.** It only writes it when you change the mode (and tries again after the HOME menu or sleep). This is the suspected cause of the white/pink full-screen flashes and the old white outlines.
- **Tap to move items, redone.** Any press that starts and ends on the same slot counts as a tap, at any speed. Tap a stack to pick it up, tap a slot to move/swap/merge, tap the held slot to put it back. Dragging to another slot works as before.
- **C-stick:**
  - **Auto-tunnel (default):** with a pickaxe, axe or hammer, pushing the C-stick steers the game's own 3-line smart dig in that direction. Weapons and blocks aim as before. Menu: "C-stick tool mining" → Precise for the old behaviour.
  - **Y is stock again:** it breaks at the game's smart cursor when the C-stick is centred. Menu: "Y button".
  - **Cursor style:** the crosshair hides when you walk without touching the C-stick and comes back on your next push. Menu: "Cursor auto-hide". Speeds are about 50% faster.
- **Fast backdrop lighting is ON by default** (about +10 fps in caves).

## Checklist
1. **OLED on, 3D on, play 10+ minutes** including bosses and busy areas: any white/pink flash or white outlines? If yes, note whether OLED was on.
2. **OLED: go to the HOME menu and back.** Is the booster still applied? (If not, re-pick it in the menu. Known risk.)
3. **Inventory and chest:** tap-pick, tap-place, swap, merge, put back. Did anything happen? Bring the card back either way: the "inv tap:" log lines show exactly what the game did.
4. **Pickaxe + C-stick push:** does it dig a tunnel in that direction? Axe on trees.
5. **Y alone:** mines like stock.
6. **Cursor style (Y+ZR):** the crosshair hides when walking and comes back on a C-stick push. Is the speed better?
7. **Caves (Fullbright off):** fps.

## Known
- Pressing B to cancel a held item may also close the page.
- Releasing a drag between two slots may pick the item up instead of cancelling.
- Auto-tunnel pauses while a grappling hook is attached and you're standing still.
- Y+ZR can switch the C-stick style if you press them together while mining.
