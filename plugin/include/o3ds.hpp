#pragma once
// Old 3DS build only (T3D_O3DS, `make o3ds`). Notes: notes/o3ds.md.
//
// Frame pacing ("30 fps lock"): nngxSwapBuffers (0x108500) shows the
// previous frame's image at the next VBlank. A slot hook at 0x1084F0 (nop,
// before WaitCmdlistDone + SwapBuffers) waits until the VBlank after the one
// the previous frame was shown at, so frames reach the screen on every second
// VBlank: an even 30 fps. A second slot at 0x108804 (after the swap) records
// the VBlank count each frame was swapped at (the real display gaps). The
// game's own timestep (GameState::Update 0x50F554: acc += real dt, 1 tick per
// 1/60 s) then runs 2 ticks per frame, so the game speed is unchanged. The
// no-catch-up patch would halve the game speed under the lock, so it only
// applies with the lock set to Off.
//
// Also: per-second pacing statistics (VBlanks per frame), the underground
// detector used by "Auto", and memory headroom for the SD log.
#include <3ds/types.h>
#include <string>

#ifdef T3D_O3DS
namespace CTRPluginFramework { struct FwkSettings; }

namespace t3d::o3ds
{
    // ---- game memory (v0.6.1, notes/o3ds.md "Memory budget") ----
    // On Old 3DS, Luma charges the plugin block to the game's commit limit,
    // and the game sizes its heaps from what is left (nninitStartUp
    // 0x108A94), so every plugin MiB is a MiB less game main heap. The
    // build therefore runs in Luma's 2 MiB block (plgInfo).
    //
    // First thing in PatchProcess (before any CTRPF System::Heap use):
    // shrink CTRPF's 1 MiB search-engine heap so the plugin fits in 2 MiB,
    // and turn the search engine off.
    void    EarlyBoot(CTRPluginFramework::FwkSettings &settings);
    // PatchProcess, after the version check, before the game starts: move
    // 1 MiB from the game's main heap to its device heap (the atlas page our
    // right-eye buffers push out of VRAM lands there).
    bool    PatchGameMemory();
    // Game thread: the device heap really has the extra MiB (else the
    // right-eye buffers must not be reserved).
    bool    GameMemoryOk();
    // Why the right-eye buffers are not reserved (2DS, SD opt-out file,
    // failed patch), or nullptr.
    const char *NoStereoReason();
    // Game thread, title (v0.7.0): would `bytes` more in VRAM-B still leave
    // room for the 2 atlas pages B holds with only the flat buffers? Reads
    // the game's VRAM allocator; false if it cannot be read.
    bool    VramRoomForRightTarget(u32 bytes);
    // Around the right target's allocation: no VRAM-B -> VRAM-A retry.
    void    BeginRightTargetAlloc();
    void    EndRightTargetAlloc();
    // Plugin thread: the numbers behind that decision ("" before it ran),
    // and the live VRAM picture for the 10 s memory line.
    std::string VramReservationLine();
    std::string VramLine();
    // Last-gasp log line on svcBreak / fatal-result paths (crash evidence).
    void    InstallPanicHooks();

    // PatchProcess(), after the version check.
    bool    Install();
    bool    Installed();

    enum LockMode { kLockAuto = 0, kLockAlways = 1, kLockOff = 2, kLockModeCount };
    void        SetLockMode(int m);     // plugin thread
    int         LockMode();
    const char *LockModeName(int m);

    // Live state (written by the game thread, read anywhere)
    bool    Engaged();                  // lock active this frame
    bool    Underground();              // Auto's cave detector (with hysteresis)
    const char *EngagedReason();        // "3D", "cave", "always", "-"

    // Plugin thread, every frame: writes queued lock / cave events to the log.
    void    FlushEvents();

    // "pace on(3D) v1 0 v2 59 v3 1 v4+ 0 wait 3.1ms ug 1"
    std::string PaceLine();
    // Bottom-screen readout: "30 lock on (3D)  60fps:0 30:58 20:1 slower:0"
    std::string ShortLine();
    // "mem app 61.2/64.0M sys 39.1/44.0M heap 1.20M used 2.31M free lin 0.00M"
    std::string MemLine();
    // One-line menu status for the lock entry, e.g. "Auto (now 30: 3D)".
    std::string LockStatus();
}
#endif
