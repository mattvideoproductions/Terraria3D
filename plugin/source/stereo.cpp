#include "stereo.hpp"
#include <3ds/os.h>

extern "C" float stereo_get_slider(void)
{
    float v = osGet3DSliderState(); // OS_SharedConfig->slider_3d, 0.0 .. 1.0
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

namespace t3d::stereo
{
    bool Enabled(float threshold)
    {
        return stereo_get_slider() > threshold;
    }

    float LayerOffset(float depth, float maxSeparationPx)
    {
        // depth: 0 = screen plane, +1 = far background, -1 = pops out.
        // Positive result = shift right for the right eye (left eye uses -result).
        return 0.5f * maxSeparationPx * stereo_get_slider() * depth;
    }
}
