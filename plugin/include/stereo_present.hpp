#pragma once
// Stereo present path, stage 1: real right-eye display buffers + whole-image
// disparity. The game's own (unused) nngx stereo swap code does the presenting;
// we only allocate/bind DISPLAY0_EXT, copy the top render target into it and
// move the right-eye scanout window with the slot-2 display-env offset.
#include <3ds/types.h>
#include <string>

namespace t3d::stereo::present
{
    enum class State { NotInstalled, WaitingForGame, Active, AllocFailed };

    // Diagnostic right-eye buffer layouts (menu "Stereo test mode")
    int         TestModeCount();
    const char *TestModeName(int mode);
    void        SetTestMode(int mode);
    int         TestMode();
    std::string DebugText();

    // Installs the main-loop slot hooks. Call from PatchProcess() after the
    // version check passed. Returns false if a hook could not be installed.
    bool    Install();

    // Call when the plugin menu opens (game paused): copies the presented
    // right-eye buffer into the other one so the menu's own buffer flips
    // never show a stale/black right eye.
    void    SyncRightBuffersForMenu();

    State   GetState();
    const char *StateString(State s);

    // User settings (menu)
    void    SetEnabled(bool on);
    bool    Enabled();
    void    SetMaxDisparity(int px);    // at full slider, clamped to [0, kPadRows]
    int     MaxDisparity();
    void    SetPerLayer(bool on);       // stage 2 per-layer depth (else stage 1 flat)
    bool    PerLayer();
    void    SetInvert(bool invert);     // flips which way "into the screen" goes
    bool    Inverted();

    // Debug info
    int     CurrentDisparity();         // rows, signed, as applied this frame
    u32     RightBufferAddr(int idx);
    bool    RightBuffersInFcram();
#ifdef T3D_O3DS
    // Old 3DS build: this frame shows a right eye (game thread, after OnPreTop).
    bool    FrameStereo();
#endif
}
