#pragma once
// Stereo stage 2: per-layer depth by "draw once, submit twice".
//
// The game's top-screen Draw() runs once, as normal. Every sprite record is
// stamped with a stable layer index (hook on SpriteBatch::NextLayer). When a
// batch is flushed into the left top render target, our End replacement lets
// the original flush draw the left eye, then replays the very same vertices
// into a second (right-eye) render target, uploading a horizontally translated
// uModelView per run of equal depth. No game state is touched twice.
// See notes/stage2_stereo.md (sections C and E).
#include <3ds/types.h>

namespace t3d::stereo::layers
{
    // Patches the layer counter, End and the command-list budget. Call once
    // from PatchProcess() after the version check passed.
    bool    Install();
    bool    Installed();

    // Increments every time the game draws the world (gameplay is on screen).
    u32     WorldDrawCount();

    // Multiplier for layers in front of the screen (particles, damage numbers,
    // gore, projectiles, UI): 0 = flat, 1 = normal, 1.75 = extra.
    void    SetPopScale(float scale);
    float   PopScale();

    // Depth presets: which layers sit where (0 = Diorama, the default).
    int         PresetCount();
    int         Preset();
    const char *PresetName(int i);
    void        SetPreset(int i);

    // Main thread, first displayed frame: create the right render target
    // while VRAM is still free (lazy creation failed on hardware in-world).
    void    ReserveRightTarget();
#ifdef T3D_O3DS
    // Old 3DS: the flat right-eye buffers were not reserved (2DS, opt-out,
    // memory patch failed, allocation failed): never create the target.
    void    RefuseRightTarget();
    u32     RightTargetBytes();     // VRAM held by the right target (0 = none)
    u32     RightTargetAddr();      // its colour buffer (0 = not created)
#endif

    // Main thread, once per frame before the top screen is drawn.
    // `on` enables the right-eye pass for this frame (lazily creating and
    // clearing the right render target). sepPx = right-eye shift in pixels for
    // depth 1.0 (negative = inverted depth).
    void    BeginFrame(bool on, float sepPx);

    // Non-zero once the right render target exists and this frame has a
    // right-eye pass: the colour resource to copy into the right display buffer.
    u32     RightColorResourceThisFrame();

    bool    RightTargetFailed();
    bool    RightTargetInFcram();
    u32     ReplayedRunsLastFrame();

    // Edge fill (default on): sprites of background layers that the shift
    // pulls away from a screen edge get a mirrored copy just off-screen, so
    // the right eye shows no bare strip. Count = extra sprites last frame.
    u32     EdgeFillsLastFrame();
    void    SetEdgeFill(bool on);
    bool    EdgeFill();

    // Stereo safe mode (hardware A/B for the white-tile bug):
    // 0 = off, 1 = replay with no shift (flat), 2 = flat shift without replay.
    int         SafeModeCount();
    const char *SafeModeName(int m);
    int         SafeMode();
    void        SetSafeMode(int m);

    // Round per-layer shifts to whole screen pixels (default on).
    void        SetSnapPixels(bool on);
    bool        SnapPixels();
}
