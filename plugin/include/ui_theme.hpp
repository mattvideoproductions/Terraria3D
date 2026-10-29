#pragma once
// Runtime UI colour themes for the bottom-screen menus (inventory, crafting,
// player, chest, main screen bar, select screens).
//
// The colours are not vertex colours or constants: every widget XML uses
// <Color>255 255 255 255</Color>, and the fills come from texels of the UI
// atlas rom:/UI.otx (512x512 RGBA4444, 8x8 Morton tiles), regions named in
// rom:/UI.pck (bg_green = one texel at 113,1 = #55AA55, bg_01..04, the -n9
// nine-patches, tabs, buttons). A theme rewrites those texels in the game's
// loaded copy of the texture, located by content (4 untouched signature tiles
// from the logo and island art). Each texel is only written when it still
// holds the value we expect, so "Stock" restores the exact original bytes.
// Nothing on disk is modified.
#include <3ds/types.h>

namespace t3d::ui_theme
{
    // PatchProcess(), after the version check. Patches nothing at boot.
    bool        Install();

    int         ThemeCount();           // 2
    const char *ThemeName(int i);       // "Stock", "Dusk (grey-purple)"
    int         Theme();
    // Applies now (synchronous memory scan on first use, ~0.1-0.5 s; call from
    // the plugin menu, the game is paused there). If the texture is not loaded
    // yet, Tick() finds it later.
    void        SetTheme(int i);

    // Game main thread, once per frame (called from stereo present OnPreTop).
    // Cheap: re-applies the theme if the game reloaded the texture in place,
    // and continues a bounded background search when the texture is missing.
    void        Tick();

    // One-line status for the menu / debug log.
    const char *Status();
}
