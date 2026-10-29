#include "memory.hpp"
#include "game_layout.hpp"

#include <cstring>
#include <CTRPluginFramework.hpp>

// Luma3DS custom SVC (exported by libctrpf's csvc.s). The *range* variant
// (svcInvalidateInstructionCacheRange) crashed the kernel on hardware with an
// "instr. cache maintenance op" data abort when it ran while another process
// (i2c) was current; Azahar does not model caches so it never showed there.
// CTRPF's own hook manager uses the whole-cache variant, so we do too.
extern "C" void svcInvalidateEntireInstructionCache(void);

using CTRPluginFramework::Process;

namespace t3d::mem
{
    static bool s_patchingAllowed = false;

    bool PatchingAllowed() { return s_patchingAllowed; }
    void SetPatchingAllowed(bool allowed) { s_patchingAllowed = allowed; }

    static bool InRange(u32 addr, u32 len, u32 start, u32 end)
    {
        return addr >= start && len <= end - start && addr - start <= (end - start) - len;
    }

    Region RegionOf(u32 addr, u32 len)
    {
        using namespace t3d::game;
        if (InRange(addr, len, kTextStart, kTextEnd)) return Region::Text;
        if (InRange(addr, len, kRoStart, kRoEnd))     return Region::RoData;
        if (InRange(addr, len, kDataStart, kDataEnd)) return Region::Data;
        return Region::None;
    }

    bool Read32(u32 addr, u32 &out)
    {
        if (RegionOf(addr, 4) == Region::None || (addr & 3))
            return false;
        out = *reinterpret_cast<const volatile u32 *>(addr);
        return true;
    }

    bool Write(u32 addr, const void *src, u32 len, void *original)
    {
        if (!s_patchingAllowed || len == 0 || src == nullptr)
            return false;

        Region r = RegionOf(addr, len);
        if (r == Region::None)
            return false;

        // CTRPF's ProtectMemory is svcControlProcess(SET_MMU_TO_RWX): the whole
        // process becomes RWX and stays so. Do it once; calling it again from
        // the plugin thread mid-game crashed (v0.3.0 dump: pc=0 inside
        // ProtectMemory while toggling "Instant mining").
        static bool s_rwx = false;
        if (r != Region::Data && !s_rwx)
        {
            Process::ProtectMemory(addr, len);
            s_rwx = true;
        }

        if (!Process::Patch(addr, const_cast<void *>(src), len, original))
            return false;

        // Process::Patch already flushed the data cache for the written range.
        if (r == Region::Text)
            svcInvalidateEntireInstructionCache();
        return true;
    }

    bool Write32(u32 addr, u32 value, u32 *original)       { return Write(addr, &value, 4, original); }
    bool Write16(u32 addr, u16 value, u16 *original)       { return Write(addr, &value, 2, original); }
    bool Write8(u32 addr, u8 value, u8 *original)          { return Write(addr, &value, 1, original); }
    bool WriteFloat(u32 addr, float value, float *original){ return Write(addr, &value, 4, original); }

    bool Replace32(u32 addr, u32 expected, u32 value)
    {
        u32 cur;
        if (!Read32(addr, cur) || cur != expected)
            return false;
        return Write32(addr, value);
    }

    bool BranchInRange(u32 from, u32 to)
    {
        s32 off = static_cast<s32>(to - (from + 8));
        return (off & 3) == 0 && off >= -0x02000000 && off <= 0x01FFFFFC;
    }

    u32 EncodeBranch(u32 from, u32 to, bool link, u32 cond)
    {
        s32 off = static_cast<s32>(to - (from + 8)) >> 2;
        return (cond << 28) | (link ? 0x0B000000u : 0x0A000000u) | (static_cast<u32>(off) & 0x00FFFFFFu);
    }

    bool PatchBranch(u32 from, u32 to, bool link)
    {
        if ((from & 3) || !BranchInRange(from, to))
            return false;
        return Write32(from, EncodeBranch(from, to, link));
    }

    bool PatchAbsJump(u32 from, u32 to)
    {
        if (from & 3)
            return false;
        const u32 code[2] = { 0xE51FF004u /* ldr pc, [pc, #-4] */, to };
        return Write(from, code, sizeof(code));
    }
}
