# Terraria 3D mod v0.5.4: test plan (New 3DS)

File: `sd/luma/plugins/000400000016A900/Terraria3D.3gx` (MD5 `F649DA02C39B35F9CE5641E7DFE08295`).
The perf log stays on. Inventory taps still write "inv tap:" lines. Benchmark with Fullbright OFF.
Tested in Azahar (focus-free) before shipping: boot, world, ZL, mining, inventory and chest taps, menu close, D-pad walking, 3D on.

## Changes
- **ZL switches the C-stick style** (Spring / Cursor). A short note on the top screen says which one ("C-stick style: Cursor").
  - There are only two styles. Spring *is* the "absolute" one (push = aim point, let go = auto-cursor), so ZL flips between Spring and Cursor.
  - **Grapple is back on A** (the game's own button). Want ZL grapple back? Controls > ZL button > Grapple, or Control preset > "New 3DS, ZL grapple".
  - The Y + ZR toggle is gone, so it can't fire while you mine.
- **Menu names are clearer:** "C-stick style" = how the aim moves (Spring / Cursor). "C-stick tool mode" = what a pickaxe does (Auto-tunnel / Precise). The Off / Aim / Auto-use entry is now "C-stick aiming".
- **Auto-tunnel starts at about half a push** (it used to need about 85%). Only for pickaxe/axe/hammer in Auto-tunnel. Weapons are unchanged. Precise never tunnels.
- **Tap to move items is fixed.** The game turns touch off on every slot while it holds an item. The mod now turns it back on.
  - Works in the inventory and inside a chest: tap an item, then tap where it goes. That moves, swaps or merges.
  - **Chest and inventory:** tap the item, tap the other tab, then tap a slot. A note says "Item still in hand".
  - To put it back, tap the same slot. B closes the page and leaves the item where it was.
- **D-pad walking** (Controls menu, off by default): the D-pad walks like the circle pad while you play. It replaces the D-pad cursor.
- **Closing the mod menu no longer leaves the game paused** (if you opened it while playing).
- Plugin info now says v0.5.4.

## Checklist
1. **ZL in play:** the note flips Spring / Cursor. ZL never opens the build view. **A** throws the grapple.
2. **Pickaxe, Auto-tunnel:** push the C-stick about half way into rock. Does it start digging sooner than before? Sword: attacks still need a hard push?
3. **Precise** (Controls > C-stick tool mode): only the aimed block breaks, never a tunnel.
4. **Inventory:** tap item → tap empty slot (move), tap item → tap other item (swap), same item type (merge), tap the same slot (put back).
5. **Chest:** inside the chest; chest → inventory (tap item, tap inventory tab, tap slot); inventory → chest.
6. **Mod menu:** open with SELECT while playing, close with SELECT. The game keeps running (no Paused screen).
7. **D-pad walking on:** the D-pad walks; inventory menus still use the D-pad normally. Turn it off again.
8. **Green lines on walls:** these are in the unmodded game too (seen in Azahar without the plugin), so expect them still.
9. 3D on, OLED on, 10+ minutes: anything new (flashes, outlines, crashes)? Bring the card back for the log.

## Known
- The half-push threshold could only be checked on real hardware (the emulator's C-stick is all-or-nothing). Tell me if it's still too stiff or now too touchy.
- If the Paused screen still shows after closing the menu, one A press resumes it. Please say how often.
- With an item "in hand" across tabs there's no floating icon, only the note.
