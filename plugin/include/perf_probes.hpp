#pragma once
// Frame-time breakdown probes (v0.4.4). Hooks on the game's update tick,
// lighting, GPU and VSync waits feed perf.cpp's per-window sections, so the
// SD log shows where a slow frame goes (the "top" timer only covers the top
// screen's draw). Addresses: notes/performance.md.
#include <3ds/types.h>
#include <string>

namespace t3d::perf::probes
{
    // PatchProcess(): install the hooks (version check passed).
    void    Install();
    // How many probe hooks are live (status box).
    int     InstalledCount();
}
