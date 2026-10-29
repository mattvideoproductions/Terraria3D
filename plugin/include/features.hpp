#pragma once
// v0.3 gameplay features: 2x zoom, dev menu / cheat console, creative mode.
// Addresses: notes/zoom_and_cheats.md and notes/creative_mode.md (v5.6.0).
#include <3ds/types.h>
#include <string>

namespace CTRPluginFramework { class PluginMenu; }

namespace t3d::features
{
    // PatchProcess(): boot-time patches and hooks (version check passed).
    void    Install();

    // Game main thread, once per frame (before the top screen is drawn).
    void    OnFrame();

    // Adds the "Zoom", "Cheats" and "Creative mode" folders.
    void    BuildMenu(CTRPluginFramework::PluginMenu &menu);

    bool    Zoom2x();

    // Plugin menu open / close (plugin thread). Closing the menu makes the
    // game open its pause page; when the menu was opened from play, the next
    // game tick presses "Return to Game" (A) once.
    void    OnMenuOpening();
    void    OnMenuClosing();

    // C-stick diagnostics: raw values from the game and the ir:rst fallback.
    std::string CstickStatus();
    bool    ShowCstickDebug();
}
