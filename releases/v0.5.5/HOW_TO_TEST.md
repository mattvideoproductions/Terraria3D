# Terraria 3D mod v0.5.5: test plan (New 3DS)

File: `sd/luma/plugins/000400000016A900/Terraria3D.3gx` (MD5 `4BF00FC97E20F93667E15CA74F3E464C`).
Everything else is the same as v0.5.4. Not run in Azahar (a menu toggle around one on-screen note, nothing else changed).

## Change
- **New menu setting "Held item hint"** (next to "Tap to move items"), on by default. It controls the "Item still in hand: tap a slot to put it there" note shown when you carry an item to another inventory tab. Off = no note. Tap to move works the same either way.
- Plugin info now says v0.5.5.

## Checklist
1. Menu shows "Held item hint" under "Tap to move items", checked by default.
2. Chest/inventory: tap an item, switch tab. The note appears. Tap a slot: the item is placed.
3. Uncheck "Held item hint", repeat: no note, item still placed.
