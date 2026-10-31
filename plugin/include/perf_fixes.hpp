#pragma once
// Game-side rendering optimisations (v5.6.0). Addresses and the tile layout
// they rely on: notes/perf.md.
//
// Cave backdrop (0x1DD048). Every cell draw of the pass goes through
// BackdropDraw (perf_fixes.cpp), which can:
//  - skip cells that solid tiles cover completely (cover cull, v0.5);
//  - skip cells whose quad lies fully off screen (viewport cull, v0.5.1);
//  - merge runs of adjacent cells with the same texture strip and colour into
//    one sprite (run merge, v0.5.1). Exact mode only merges identical colours,
//    so the picture is unchanged; "fast lighting" also merges near-equal ones.
// Each is conservative: anything uncertain is drawn as stock.
#include <3ds/types.h>
#include <string>

namespace t3d::perf_fixes
{
    // PatchProcess(), after features::Install() (version check passed).
    void    Install();

    // Menu toggles. The patches stay in place; OFF passes every draw through.
    void    SetBackdropCull(bool on);   // cover cull
    bool    BackdropCull();
    void    SetViewportCull(bool on);   // off-screen cull
    void    SetMergeRuns(bool on);      // exact run merge
    void    SetFastLighting(bool on);   // near-equal colours merge too (visible, slight)

    // True when all backdrop patch sites were installed.
    bool    BackdropCullInstalled();

    // Called by the 0x1DD048 probe around the original pass (perf_probes.cpp).
    // Run merging needs the end call to flush its last pending run, so it
    // stays off unless SetPassHooked(true) was called.
    void    SetPassHooked(bool on);
    void    BackdropPassBegin();
    void    BackdropPassEnd();

    // Per-pass averages since the last call, for the SD log.
    // "bg in 812 cov 0 vp 290 mrg 160 out 362"
    std::string TakeStats();

    // "No catch-up": at most one game tick per frame (GameState::Update loop
    // branch 0x50FAA4). Slow frames then run the game in slow motion instead of
    // spending a second tick catching up. Default OFF.
    void    SetNoCatchUp(bool on);
    bool    NoCatchUp();
#ifdef T3D_O3DS
    // Main object the backdrop pass last ran with (0 = never ran yet).
    u32     DrawMain();
    void    ClearDrawMain();          // game thread, out of a world
#endif
}
