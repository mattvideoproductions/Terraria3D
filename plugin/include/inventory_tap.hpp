#pragma once
// "Tap to pick, tap to place" for the bottom-screen item grids (inventory,
// chest, piggy bank, safe ...), like the PS Vita version. Addresses and the
// evidence for them: notes/inventory.md (v5.6.0).
//
// It never touches item memory. A tap on a slot (quick tap, or a press that
// is released on the slot it started on) runs the game's own "A button on
// the focused slot" handler for that grid: pick up into the grid's carry
// mode, or place / merge / swap from it, the same code the D-pad + A path
// uses. Tapping the carried slot again or pressing B leaves carry mode
// through the game's own cancel. Dragging to another slot is untouched.
#include <3ds/types.h>

namespace t3d::inventory_tap
{
    // PatchProcess() (version check passed).
    void    Install();

    // Menu toggle (plugin thread). Off = every hook just calls the game.
    void    SetEnabled(bool on);
    bool    Enabled();

    // Menu toggle: the "Item still in hand" note after a tab switch (v0.5.5).
    void    SetHintEnabled(bool on);

    // True when all hooks are in place.
    bool    Installed();

    // Plugin thread, every frame: writes the queued "inv tap:" log lines
    // (picked / placed / cancelled / ignored + reason) to the perf log.
    void    FlushLog();
}
