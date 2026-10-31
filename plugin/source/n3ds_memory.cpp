// New 3DS build only: device heap headroom (v0.5.6 crash fix) and memory
// telemetry. See n3ds_memory.hpp. The Old 3DS build keeps its own copy of
// the heap walker and panic hooks in o3ds.cpp, so its binary is unchanged.
#include "n3ds_memory.hpp"

#ifndef T3D_O3DS

#include <CTRPluginFramework.hpp>
#include <cstdio>

#include "hooks.hpp"
#include "memory.hpp"
#include "perf.hpp"

using namespace CTRPluginFramework;

namespace t3d::n3ds
{
    namespace
    {
        // nninitStartUp 0x108A94: device = 0x1E80000, or 0x4900000 when
        // 0x113BC4 says New 3DS (this movne); main = (APPMEMALLOC -
        // committed) - device, capped at 96 MiB.
        constexpr u32 kDevSizeInsnAddr = 0x00108AB4;
        constexpr u32 kDevSizeInsnOrig = 0x13A04649;    // movne r4, #0x4900000
#ifdef T3D_TEST_SIM_N3DS_CHARGE
        // EMULATOR TEST BUILDS ONLY: device +9 MiB, i.e. main another 5 MiB
        // lower, as if the console charged Luma's block a second time.
        constexpr u32 kDevSizeInsnNew  = 0x13A04652;    // movne r4, #0x5200000
#else
        constexpr u32 kDevSizeInsnNew  = 0x13A0464D;    // movne r4, #0x4D00000 (+4 MiB)
#endif
        // Game heap table (0x633DB0): +0x08 main ExpHeap*, +0x14 device ExpHeap*.
        constexpr u32 kHeapTable       = 0x00633DB0;
        constexpr u32 kExpMagic        = 0x45585048;    // "HPXE" at +0x18
        constexpr u16 kFreeMagic       = 0x4652;        // free block "RF"

        template <typename T> inline T Rd(u32 a) { return *reinterpret_cast<const volatile T *>(a); }

        bool s_memPatched = false;

        struct HeapStat { u32 size, freeBytes, largest, blocks; bool ok; };

        // Address ranges a heap's memory may lie in: main (heap region) and
        // device (linear FCRAM, 0x30000000 + up to 256 MiB).
        constexpr u32 kMainLo = 0x08000000, kMainHi = 0x10000000;
        constexpr u32 kLinLo  = 0x30000000, kLinHi  = 0x40000000;

        // Reads only; the header and every pointer are range-checked before
        // they are followed, so a list changing under us (or a corrupt
        // header) can only give a wrong number, never a data abort.
        HeapStat WalkHeap(u32 heap, u32 lo, u32 hi)
        {
            HeapStat h = {};
            if (heap < kMainLo || heap >= kMainHi - 0x40 || (heap & 3) || Rd<u32>(heap + 0x18) != kExpMagic)
                return h;
            const u32 start = Rd<u32>(heap + 0x30), end = Rd<u32>(heap + 0x34);
            if (start < lo || end > hi || end <= start || end - start > 0x8000000)
                return h;
            h.size = end - start;
            h.ok = true;
            u32 b = Rd<u32>(heap + 0x3C);
            while (b && h.blocks < 4096)
            {
                if ((b & 3) || b < start || b > end - 0x10 || Rd<u16>(b) != kFreeMagic)
                {
                    h.ok = false;
                    break;
                }
                const u32 sz = Rd<u32>(b + 4);
                h.freeBytes += sz;
                if (sz > h.largest)
                    h.largest = sz;
                h.blocks++;
                b = Rd<u32>(b + 0xC);
            }
            return h;
        }

        u32 MainHeapObj()   { return Rd<u32>(kHeapTable + 0x08); }
        u32 DeviceHeapObj() { return Rd<u32>(kHeapTable + 0x14); }

        HeapStat MainHeap()   { return WalkHeap(MainHeapObj(), kMainLo, kMainHi); }
        HeapStat DeviceHeap() { return WalkHeap(DeviceHeapObj(), kLinLo, kLinHi); }

        // Minima since boot. A walk can tear while the game frees and
        // allocates (one RPC walk read 0.04 MiB free), so a reading only
        // counts toward the minimum when the previous one was as low too.
        u32 s_minMainFree = 0xFFFFFFFF, s_minDevFree = 0xFFFFFFFF;
        u32 s_prevMainFree = 0xFFFFFFFF, s_prevDevFree = 0xFFFFFFFF;

        void TrackMin(const HeapStat &h, u32 &prev, u32 &minFree)
        {
            if (!h.ok)
                return;
            const u32 confirmed = h.freeBytes > prev ? h.freeBytes : prev;
            if (confirmed < minFree)
                minFree = confirmed;
            prev = h.freeBytes;
        }

        // ---- panic hooks: one line straight to SD from the dying thread ----
        constexpr u32 kFnSvcBreak     = 0x0013722C;     // svc 0x3C ; bx lr (abort / terminate path)
        constexpr u32 kInsnSvcBreak   = 0xEF00003C;
        constexpr u32 kFnFatalResult  = 0x0013B16C;     // (Result, caller pc): failed-result handler (ERR:F)
        constexpr u32 kInsnFatalRes   = 0xE1A02000;     // mov r2, r0
        hook::FunctionHook s_breakHook, s_fatalHook;
        volatile u32 s_panicLines = 0;
        char s_panicBuf[320];

        // The game's fatal path (0x145CB0) tail-calls svcBreak with r0 = r1
        // = 0, so the arguments say nothing. The callers' return addresses
        // are still on the stack: collect up to 4 words that point into the
        // game's .text, scanning only the rest of the current 4 KiB stack
        // page (always mapped) above this frame.
        constexpr u32 kTextLo = 0x00100000, kTextHi = 0x005C0BBC;

        void StackCallers(u32 out[4])
        {
            volatile u32 marker = 0;        // lives on this thread's stack
            u32 sp = (reinterpret_cast<u32>(&marker) + 4) & ~3u;
            const u32 pageEnd = (sp | 0xFFFu) + 1;
            int n = 0;
            for (; sp < pageEnd && n < 4; sp += 4)
            {
                const u32 w = Rd<u32>(sp);
                if (w >= kTextLo && w < kTextHi)
                    out[n++] = w;
            }
            for (; n < 4; n++)
                out[n] = 0;
        }

        void PanicLine(const char *what, u32 a, u32 b)
        {
            if (__atomic_fetch_add(&s_panicLines, 1, __ATOMIC_ACQ_REL) >= 4)
                return;     // never flood the SD from a failing loop
            u32 c[4];
            StackCallers(c);
            const HeapStat m = MainHeap(), d = DeviceHeap();
            constexpr double M = 1024.0 * 1024.0;
            snprintf(s_panicBuf, sizeof(s_panicBuf),
                     "PANIC %s %08lX %08lX stack %08lX %08lX %08lX %08lX | game main free %.2fM largest %.2fM of %.2fM%s, dev free %.2fM largest %.2fM of %.2fM%s",
                     what, a, b, c[0], c[1], c[2], c[3],
                     m.freeBytes / M, m.largest / M, m.size / M, m.ok ? "" : "?",
                     d.freeBytes / M, d.largest / M, d.size / M, d.ok ? "" : "?");
            perf::PanicWrite(s_panicBuf);
        }

        u32 OnSvcBreak(u32 r0, u32 r1, u32 r2, u32 r3)
        {
            PanicLine("svcBreak", r0, r1);
            return hook::FunctionHook::CallOriginal<u32>(r0, r1, r2, r3);
        }

        void OnFatalResult(u32 result, u32 pc)
        {
            PanicLine("fatal result at", result, pc);
            hook::FunctionHook::CallOriginal<void>(result, pc);
        }
    }

    bool PatchGameMemory()
    {
        // Runs before the game executes (PatchProcess), so nninitStartUp
        // sees the new size. On an Old 3DS running this build the patched
        // movne is never taken, so nothing changes there.
        s_memPatched = mem::Replace32(kDevSizeInsnAddr, kDevSizeInsnOrig, kDevSizeInsnNew);
        perf::Log(std::string("memory: game device heap ") +
#ifdef T3D_TEST_SIM_N3DS_CHARGE
                  (s_memPatched ? "TEST BUILD +9 MiB (simulates a second 5 MiB plugin charge on main)"
#else
                  (s_memPatched ? "+4 MiB (main heap -4 MiB): room for the game's textures"
#endif
                                : "patch FAILED (unexpected instruction): stock heap sizes"));
        return s_memPatched;
    }

    void InstallPanicHooks()
    {
        const hook::Result a = s_breakHook.Install(kFnSvcBreak, reinterpret_cast<void *>(OnSvcBreak), kInsnSvcBreak);
        const hook::Result b = s_fatalHook.Install(kFnFatalResult, reinterpret_cast<void *>(OnFatalResult), kInsnFatalRes);
        perf::Log(std::string("panic log hooks: break ") + hook::ResultString(a) + ", fatal " + hook::ResultString(b));
    }

    std::string MemLine()
    {
        const HeapStat m = MainHeap(), d = DeviceHeap();
        TrackMin(m, s_prevMainFree, s_minMainFree);
        TrackMin(d, s_prevDevFree, s_minDevFree);
        constexpr double M = 1024.0 * 1024.0;
        auto mn = [](u32 v) { return v == 0xFFFFFFFF ? 0.0 : v / (1024.0 * 1024.0); };
        return Utils::Format("mem game main free %.2fM (min %.2fM) largest %.2fM of %.2fM%s, dev free %.2fM (min %.2fM) largest %.2fM of %.2fM%s",
                             m.freeBytes / M, mn(s_minMainFree), m.largest / M, m.size / M, m.ok ? "" : "?",
                             d.freeBytes / M, mn(s_minDevFree), d.largest / M, d.size / M, d.ok ? "" : "?");
    }
}

#endif // !T3D_O3DS
