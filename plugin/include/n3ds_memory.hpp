#pragma once
// New 3DS build only (the Old 3DS build has its own, see o3ds.hpp).
//
// v0.5.6 crash fix. The game's device heap (linear FCRAM, 73 MiB on New 3DS)
// is nearly full by design: stock leaves 3.24 MiB free at the title, and the
// world map keeps a 128x128 texture per revealed chunk in it, created on
// demand (0x26ABF0, FCRAM only) and never freed during a session. Our
// right-eye VRAM pushes atlas pages from VRAM-B into the same heap (1.74 MiB
// free left), so after enough exploring, opening the map failed a texture
// allocation and the game called svcBreak (hardware dump, 2026-10-31).
// Fix: nninitStartUp gives the device heap 4 MiB more, taken from the main
// heap (~18 MiB free in-world with the plugin).
#include <3ds/types.h>
#include <string>

#ifndef T3D_O3DS
namespace t3d::n3ds
{
    // PatchProcess, after the version check, before the game runs
    // nninitStartUp.
    bool    PatchGameMemory();
    // svcBreak / fatal-result hooks: one PANIC line with the heaps, straight
    // to the SD card from the dying thread.
    void    InstallPanicHooks();
    // "mem game main free 12.3M (min 11.9M) largest 11.0M of 33.8M, dev ..."
    // Plugin thread (walks the game's heap free lists, read only).
    std::string MemLine();
}
#endif
