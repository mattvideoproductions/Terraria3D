#pragma once
// Memory write helpers for the game's .text / .rodata / .data(+bss).
//
// All writers are LOCKED until t3d::version::Verify() succeeds, so nothing is ever
// written into an unsupported game version. Writes into .text flush the data
// cache and invalidate the instruction cache for the patched range.
#include <3ds/types.h>

namespace t3d::mem
{
    enum class Region { None, Text, RoData, Data };

    Region  RegionOf(u32 addr, u32 len = 1);
    bool    PatchingAllowed();
    void    SetPatchingAllowed(bool allowed); // called by version::Verify()

    // Raw reads (no lock; reading is always safe inside the game's segments)
    bool    Read32(u32 addr, u32 &out);

    // Generic write. `original` (optional) receives the bytes that were there.
    bool    Write(u32 addr, const void *src, u32 len, void *original = nullptr);
    bool    Write32(u32 addr, u32 value, u32 *original = nullptr);
    bool    Write16(u32 addr, u16 value, u16 *original = nullptr);
    bool    Write8(u32 addr, u8 value, u8 *original = nullptr);
    bool    WriteFloat(u32 addr, float value, float *original = nullptr);

    // Write only if the current word equals `expected` (per-patch guard).
    bool    Replace32(u32 addr, u32 expected, u32 value);

    // ---- ARM-mode branch helpers ----
    // B/BL reach is +-32 MiB. The plugin lives at 0x07000000, which is NOT
    // reachable from game .text (0x100000..0x5C0BBC); use PatchAbsJump or
    // t3d::hook::FunctionHook to reach plugin code.
    bool    BranchInRange(u32 from, u32 to);
    u32     EncodeBranch(u32 from, u32 to, bool link, u32 cond = 0xE);
    bool    PatchBranch(u32 from, u32 to, bool link = false);

    // 8-byte absolute jump: `ldr pc, [pc, #-4]` + `.word to`.
    // Overwrites 2 instructions at `from`; use at a function entry to replace it.
    bool    PatchAbsJump(u32 from, u32 to);

    // ARM `bx lr` / `mov r0,#imm8` helpers for quick stubbing
    constexpr u32 kInsnBxLr = 0xE12FFF1E;
    constexpr u32 kInsnNop  = 0xE320F000;
    constexpr u32 MovR0Imm(u8 imm) { return 0xE3A00000u | imm; }
}
