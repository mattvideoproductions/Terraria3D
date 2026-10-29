#pragma once
#include <3ds/types.h>

// 3D slider position in [0.0, 1.0] (0 = 3D off). Reads the OS shared config page
// (0x1FF81080 + 0x0, slider_3d) via libctru's osGet3DSliderState().
extern "C" float stereo_get_slider(void);

namespace t3d::stereo
{
    // True when the slider is up far enough that stereo rendering should be on.
    bool    Enabled(float threshold = 0.01f);

    // Placeholder for the per-layer eye offset model (px at full slider).
    // Will be driven by the world-draw hooks once they are found.
    float   LayerOffset(float depth, float maxSeparationPx = 10.0f);
}
