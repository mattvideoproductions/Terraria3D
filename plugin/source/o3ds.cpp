// Old 3DS build only: frame pacing, cave detector, pacing / memory telemetry.
// See o3ds.hpp and notes/o3ds.md.
#include "o3ds.hpp"

#ifdef T3D_O3DS

#include <CTRPluginFramework.hpp>
#include <malloc.h>
#include <cstdio>
#include <new>
extern "C" {
#include <3ds/services/cfgu.h>   // no extern "C" guard of its own
}

#include "hooks.hpp"
#include "memory.hpp"
#include "perf.hpp"
#include "perf_fixes.hpp"
#include "stereo_layers.hpp"
#include "stereo_present.hpp"

extern "C" {
    extern char *fake_heap_start;   // newlib heap of the plugin (libctru allocateHeaps)
    extern char *fake_heap_end;
    extern u32 __ctru_linear_heap_size;
}

// CTRPF's System::Heap block (pluginInit.cpp InitHeap: 1 MiB from the newlib
// heap, "mostly used for Search"); its pool is set up on first use.
namespace CTRPluginFramework::Heap
{
    extern u8  *__ctrpf_heap;
    extern u32  __ctrpf_heap_size;
}

using namespace CTRPluginFramework;

// ---- CTRPF's 1 MiB System::Heap in a 2 MiB plugin block (v0.6.1) ----
// pluginInit.cpp InitHeap() runs `::operator new(0x100000)` before
// PatchProcess. In Luma's 2 MiB block the newlib heap is only ~0.5 MiB, so
// that request would abort the plugin. The first 1 MiB request before
// EarlyBoot() gets this static block instead; EarlyBoot() then sets the
// matching size before the pool is created (on its first use). CTRPF never
// frees that block; operator delete ignores it anyway.
namespace
{
    constexpr size_t kCtrpfHeapRequest = 0x100000;
    constexpr size_t kCtrpfSmallHeap   = 0x20000;   // directory listings (55 KiB) + line buffers
    alignas(16) u8   s_ctrpfSmallHeap[kCtrpfSmallHeap];
    bool             s_ctrpfRequestServed = false;
    bool             s_earlyBootDone = false;

    inline bool IsSmallHeap(void *p)
    {
        return p >= static_cast<void *>(s_ctrpfSmallHeap) && p < static_cast<void *>(s_ctrpfSmallHeap + kCtrpfSmallHeap);
    }
}

void *operator new(size_t n)
{
    if (!s_earlyBootDone && !s_ctrpfRequestServed && n == kCtrpfHeapRequest)
    {
        s_ctrpfRequestServed = true;
        return s_ctrpfSmallHeap;
    }
    if (n == 0)
        n = 1;
    void *p = malloc(n);
    if (!p)
        abort();    // what the library operator new does without exceptions
    return p;
}
void *operator new[](size_t n)                          { return ::operator new(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept   { return n ? malloc(n) : malloc(1); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return n ? malloc(n) : malloc(1); }
void operator delete(void *p) noexcept                  { if (p && !IsSmallHeap(p)) free(p); }
void operator delete[](void *p) noexcept                { ::operator delete(p); }
void operator delete(void *p, size_t) noexcept          { ::operator delete(p); }
void operator delete[](void *p, size_t) noexcept        { ::operator delete(p); }

namespace t3d::o3ds
{
    namespace
    {
        // ---- game addresses (v5.6.0), notes/o3ds.md ----
        // Main loop, per frame:
        //   0x1084e8 bl nngxSplitDrawCmdlist
        //   0x1084ec nop
        //   0x1084f0 nop            <- kPaceSlot: wait here
        //   0x1084f4 bl nngxWaitCmdlistDone (no arguments)
        //   0x1084f8 ldr r0,=0x402 ; 0x108500 bl nngxSwapBuffers
        //   ...      0x108804 nop   <- kMeasureSlot (every path to the flag clear)
        //   0x108930 strb #0 -> frame-ready flag 0x633B58; b 0x107f20
        // SwapBuffers shows the previous frame's image at the next VBlank, so
        // waiting BEFORE it paces what reaches the screen (a wait after it
        // only paced command-list kicks: audit 2026-10-29). Both slots sit
        // right before a `bl`: r0-r3, r12, flags and s0-s15 are dead there.
        constexpr u32 kPaceSlot      = 0x001084F0;
        constexpr u32 kMeasureSlot   = 0x00108804;
        // nngx top-screen VBlank counter (state 0x8C8208 + 0x154), bumped by
        // the VBlank handler 0x1174DC after it ran the game's callback.
        constexpr u32 kVBlankCount   = 0x008C835C;
        // Underground detector: world-surface depth in world pixels (int)
        // [M: 0x1DD048 compares it with Main+0x104 + Main+0x10C and starts the
        // cave backdrop there], maxTilesY (s16).
        constexpr u32 kSurfacePx     = 0x00633440;
        constexpr u32 kMaxTilesY     = 0x006333A8;
        constexpr u32 kMainScreenY   = 0x114;       // s32 screenPosition.Y (world px)
        constexpr u32 kMainViewH     = 0x10C;       // s32 view height (world px) [M]

        constexpr double kTicksPerMs = 268111.856;
        constexpr u64 kTicks(double ms) { return static_cast<u64>(ms * kTicksPerMs); }

        inline u32 VCount() { return *reinterpret_cast<volatile u32 *>(kVBlankCount); }
        template <typename T> inline T Rd(u32 a) { return *reinterpret_cast<const volatile T *>(a); }

        hook::SlotHook s_paceHook, s_measureHook;
        bool           s_installed = false, s_measureInstalled = false;

        volatile int   s_mode = kLockAuto;
        volatile bool  s_engaged = false;
        volatile u8    s_reason = 0;                // 0 none, 1 3D, 2 cave, 3 always
        volatile bool  s_underground = false;

        // game thread state
        bool  s_haveLast = false;
        u32   s_lastShownV = 0;                     // VBlank count right after the last swap
        u64   s_lastSwapTick = 0;
        u64   s_pendingWait = 0;                    // this frame's wait, for the window stats
        u32   s_lastWorldDraws = 0;
        u32   s_noWorldFrames = 0;
        u32   s_ugFrames = 0;                       // frames the candidate disagreed with the state
        u32   s_stereoHold = 0;                     // frames the 3D reason outlives the last stereo frame
        constexpr u32 kUgSwitchFrames  = 45;        // 0.75-1.5 s before the cave state flips
        constexpr u32 kUgMarginPx      = 64;        // 4 tiles of hysteresis around the surface line
        constexpr u32 kStereoHoldFrames = 30;       // 3D off for ~1 s before "3D" stops locking

        // window accumulators (game thread)
        u32   w_hist[5] = {};                       // VBlanks between shown frames: 0, 1, 2, 3, 4+
        u32   w_frames = 0, w_engagedFrames = 0, w_timeouts = 0;
        u64   w_wait = 0, w_start = 0;

        struct PaceStats
        {
            u32   hist[5];
            u32   frames, engagedFrames, timeouts;
            float waitMs;
        };
        // Double buffer: the game thread fills the slot the reader is not
        // pointed at, then flips the index (one store) once a second.
        PaceStats      s_pub[2] = {};
        volatile u32   s_pubIdx = 0;

        // ---- events: game thread -> plugin thread (single producer / consumer) ----
        enum EvType : u8 { kEvLockOn = 1, kEvLockOff, kEvCaveIn, kEvCaveOut, kEvTimeout, kEvSuppressed };
        struct Event { u8 type, reason; u16 pad; u32 value; float sinceBootS; };
        constexpr u32 kEvCount = 32;
        Event          s_ev[kEvCount];
        u32            s_evHead = 0, s_evTail = 0;  // atomics (release / acquire)
        u64            s_bootTick = 0;
        u64            s_lastTimeoutEv = 0, s_lastLockEv = 0;
        u32            s_suppressedLockEv = 0;

        void Push(u8 type, u8 reason, u32 value)
        {
            const u32 h = s_evHead;
            if (h - __atomic_load_n(&s_evTail, __ATOMIC_ACQUIRE) >= kEvCount)
                return;     // full: drop (the per-second counters still show it)
            Event &e = s_ev[h % kEvCount];
            e.type = type;
            e.reason = reason;
            e.value = value;
            e.sinceBootS = static_cast<float>((svcGetSystemTick() - s_bootTick) / (kTicksPerMs * 1000.0));
            __atomic_store_n(&s_evHead, h + 1, __ATOMIC_RELEASE);
        }

        // Lock on/off events at most every 2 s; the rest are counted and
        // reported with the next one (LESSONS: rate-limit flapping).
        void PushLock(bool on, u8 reason, u64 now)
        {
            if (s_lastLockEv && now - s_lastLockEv < kTicks(2000.0))
            {
                s_suppressedLockEv++;
                return;
            }
            if (s_suppressedLockEv)
            {
                Push(kEvSuppressed, 0, s_suppressedLockEv);
                s_suppressedLockEv = 0;
            }
            s_lastLockEv = now;
            Push(on ? kEvLockOn : kEvLockOff, reason, 0);
        }

        const char *ReasonName(u8 r)
        {
            switch (r)
            {
                case 1: return "3D";
                case 2: return "cave";
                case 3: return "always";
            }
            return "-";
        }

        // -1 = unknown (values implausible), 0 = surface, 1 = underground
        int UndergroundCandidate(int state)
        {
            const u32 main = perf_fixes::DrawMain();
            if (!main)
                return 0;   // the cave backdrop hasn't drawn in this world: surface
            const s32 surf = Rd<s32>(kSurfacePx);
            const s32 maxY = Rd<s16>(kMaxTilesY);
            if (maxY <= 0 || surf <= 0 || surf >= maxY * 16)
                return -1;
            s32 viewH = Rd<s32>(main + kMainViewH);
            if (viewH < 120 || viewH > 1200)
                viewH = 480;
            const s32 centre = Rd<s32>(main + kMainScreenY) + viewH / 2;
            const s32 margin = state ? -static_cast<s32>(kUgMarginPx) : static_cast<s32>(kUgMarginPx);
            return centre > surf + margin ? 1 : 0;
        }

        void UpdateUnderground()
        {
            const u32 draws = stereo::layers::WorldDrawCount();
            const bool inWorld = draws != s_lastWorldDraws;
            s_lastWorldDraws = draws;
            if (!inWorld)
            {
                // Menus, loading, the world map: forget the cave after a
                // moment, and the backdrop's Main pointer with it (the next
                // world records it again when its backdrop draws).
                if (++s_noWorldFrames >= 30)
                {
                    perf_fixes::ClearDrawMain();
                    if (s_underground)
                    {
                        s_underground = false;
                        s_ugFrames = 0;
                        Push(kEvCaveOut, 0, 0);
                    }
                }
                return;
            }
            s_noWorldFrames = 0;
            const int cand = UndergroundCandidate(s_underground ? 1 : 0);
            if (cand < 0 || (cand == 1) == s_underground)
            {
                s_ugFrames = 0;
                return;
            }
            if (++s_ugFrames >= kUgSwitchFrames)
            {
                s_underground = cand == 1;
                s_ugFrames = 0;
                Push(s_underground ? kEvCaveIn : kEvCaveOut, 0, 0);
            }
        }

        // Game main thread, before WaitCmdlistDone + SwapBuffers.
        void OnPaceSlot()
        {
            const u64 t0 = svcGetSystemTick();
            UpdateUnderground();

            // 3D engages at once, and releases only after ~1 s without a
            // right eye (no flapping on a single skipped stereo frame).
            if (stereo::present::FrameStereo())
                s_stereoHold = kStereoHoldFrames;
            else if (s_stereoHold)
                s_stereoHold--;

            const int mode = s_mode;
            u8 reason = 0;
            if (mode == kLockAlways)
                reason = 3;
            else if (mode == kLockAuto)
            {
                if (s_stereoHold)
                    reason = 1;
                else if (s_underground)
                    reason = 2;
            }
            const bool eng = reason != 0;
            if (eng != s_engaged)
                PushLock(eng, eng ? reason : s_reason, t0);
            s_engaged = eng;
            s_reason = reason;

            // The previous frame was shown at lastShownV + 1. Swap no earlier
            // than VBlank lastShownV + 2, so this one shows at >= lastShownV + 3.
            // Never wait on stale timing after a long gap (HOME, sleep,
            // loading, plugin menu).
            s_pendingWait = 0;
            if (eng && s_haveLast && t0 - s_lastSwapTick < kTicks(100.0))
            {
                const u32 target = s_lastShownV + 2;
                while (static_cast<s32>(VCount() - target) < 0)
                {
                    if (svcGetSystemTick() - t0 > kTicks(40.0))
                    {
                        // VBlanks stopped (should not happen): give up
                        w_timeouts++;
                        if (t0 - s_lastTimeoutEv > kTicks(5000.0))
                        {
                            s_lastTimeoutEv = t0;
                            Push(kEvTimeout, 0, VCount() - s_lastShownV);
                        }
                        break;
                    }
                    svcSleepThread(1000000);    // 1 ms; the swap has ~15 ms to the next VBlank
                }
                s_pendingWait = svcGetSystemTick() - t0;
                perf::ShiftTopDoneTick(s_pendingWait);   // keep the wait out of "bot"
            }
            if (!s_measureInstalled)
            {
                // Fallback: measure here (misses a swap pushed late by the GPU wait)
                s_lastShownV = VCount();
                s_lastSwapTick = svcGetSystemTick();
            }
        }

        // Game main thread, right after SwapBuffers: the VBlank count now is
        // the one the swapped frame shows after, so deltas are real display gaps.
        void Measure(u32 v, u64 now)
        {
            if (s_haveLast)
            {
                const u32 d = v - s_lastShownV;
                w_hist[d >= 4 ? 4 : d]++;
            }
            s_lastShownV = v;
            s_lastSwapTick = now;
            s_haveLast = true;

            w_frames++;
            if (s_engaged)
                w_engagedFrames++;
            w_wait += s_pendingWait;
            s_pendingWait = 0;
            if (!w_start)
                w_start = now;
            if (now - w_start >= kTicks(1000.0))
            {
                const u32 slot = s_pubIdx ^ 1;
                PaceStats &p = s_pub[slot];
                for (int i = 0; i < 5; i++) { p.hist[i] = w_hist[i]; w_hist[i] = 0; }
                p.frames = w_frames;
                p.engagedFrames = w_engagedFrames;
                p.timeouts = w_timeouts;
                p.waitMs = w_frames ? static_cast<float>((w_wait / kTicksPerMs) / w_frames) : 0.0f;
                __atomic_store_n(&s_pubIdx, slot, __ATOMIC_RELEASE);
                w_frames = w_engagedFrames = w_timeouts = 0;
                w_wait = 0;
                w_start = now;
            }
        }

        void OnMeasureSlot()
        {
            Measure(VCount(), svcGetSystemTick());
        }

        // Memory minima since boot (plugin thread)
        s64 s_minAppFree = -1, s_minSysFree = -1;

        // ---- game memory (v0.6.1) ----
        // nninitStartUp 0x108A94: device = 0x1E80000 (Old 3DS; 0x4900000 on
        // New 3DS), main heap = (APPMEMALLOC - committed) - device. The
        // device heap (linear FCRAM) holds the textures that do not fit in
        // VRAM and the sound buffers. Stock VRAM is 100% used once the title
        // has loaded its atlases (Azahar, VRAM allocator 0x8B8000: A 11.9 KiB
        // free, B 0), so our 607.5 KiB of right-eye buffers in VRAM-B push one
        // 1 MiB atlas page into the device heap, which has ~1.3 MiB spare.
        constexpr u32 kDevSizeInsnAddr = 0x00108AA8;
        constexpr u32 kDevSizeInsnOrig = 0xE3A0477A;    // mov r4, #0x1E80000
        constexpr u32 kDevSizeInsnNew  = 0xE3A0477E;    // mov r4, #0x1F80000 (+1 MiB)
        constexpr u32 kDevSizeNew      = 0x1F80000;
        // Game heap table (0x633DB0): +0x08 main ExpHeap*, +0x14 device ExpHeap*.
        constexpr u32 kHeapTable       = 0x00633DB0;
        constexpr u32 kExpMagic        = 0x45585048;    // "HPXE" at +0x18
        constexpr u16 kFreeMagic       = 0x4652;        // free block "RF"

        bool s_memPatched = false;
        const char *s_noStereoReason = nullptr;
        // Present on the SD card = no 3D, the game keeps its whole memory.
        constexpr const char *kNo3dFile = "/luma/plugins/000400000016A900/t3d_no3d.txt";
        bool s_ctrpfHeapShrunk = false;
        u32  s_ctrpfHeapOld = 0, s_ctrpfHeapNew = 0;

        struct HeapStat { u32 size, freeBytes, largest, blocks; bool ok; };

        // Reads only; every pointer is range-checked before it is followed,
        // so a list changing under us can only give a wrong number.
        HeapStat WalkHeap(u32 heap)
        {
            HeapStat h = {};
            if (heap < 0x08000000 || heap >= 0x10000000 || (heap & 3) || Rd<u32>(heap + 0x18) != kExpMagic)
                return h;
            const u32 start = Rd<u32>(heap + 0x30), end = Rd<u32>(heap + 0x34);
            if (end <= start)
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

        // ---- the game's VRAM allocator (v0.7.0), read only ----
        // 0x8B8000: four trees (used A +0x00, used B +0x1C, free A +0x38,
        // free B +0x54); tree +0x10 = header node, header +4 = root; node =
        // {?, parent, left, right, addr, size & 0xFFFFFF} (tools/azahar_vram.py).
        // The atlas pages are exactly 1 MiB blocks. Stock: A holds 1 page,
        // B holds 3 (VRAM is 100% used after the title). Our right-eye VRAM
        // (all in B) must leave room for 2 of them, i.e. displace one page,
        // which the +1 MiB device heap pays for.
        constexpr u32 kVramAlloc  = 0x008B8000;
        constexpr u32 kVramUsedA  = 0x00, kVramUsedB = 0x1C, kVramFreeA = 0x38, kVramFreeB = 0x54;
        constexpr u32 kVramPage   = 0x100000;
        constexpr u32 kVramPagesB = 2;          // pages B must still hold (v0.6.1 layout)

        struct VramStat { u32 bytes, largest, pages, blocks; bool ok; };
        struct VramBlock { u32 addr, size; };
        constexpr int kMaxFreeBlocks = 32;

        // Every node pointer must lie inside the game's main ExpHeap (where
        // the allocator keeps its nodes) before it is read; bounded walk.
        // Optionally collects the blocks (addr, size); more than `cap`
        // blocks makes the walk not ok.
        VramStat WalkVram(u32 tree, VramBlock *out = nullptr, int cap = 0)
        {
            VramStat v = {};
            const u32 heap = MainHeapObj();
            if (heap < 0x08000000 || heap >= 0x10000000 || (heap & 3) || Rd<u32>(heap + 0x18) != kExpMagic)
                return v;
            const u32 lo = Rd<u32>(heap + 0x30), hi = Rd<u32>(heap + 0x34);
            auto nodeOk = [&](u32 n) { return !(n & 3) && n >= lo && hi >= 0x18 && n <= hi - 0x18; };
            const u32 hdr = Rd<u32>(tree + 0x10);
            if (!nodeOk(hdr))
                return v;
            constexpr int kStack = 64;
            u32 stack[kStack];
            int sp = 0;
            stack[sp++] = Rd<u32>(hdr + 4);
            while (sp > 0)
            {
                const u32 n = stack[--sp];
                if (n == 0 || n == hdr)
                    continue;
                if (!nodeOk(n) || v.blocks >= 1024 || sp + 2 > kStack)
                    return v;           // ok stays false
                const u32 sz = Rd<u32>(n + 0x14) & 0xFFFFFF;
                if (out)
                {
                    if (static_cast<int>(v.blocks) >= cap)
                        return v;       // ok stays false
                    out[v.blocks].addr = Rd<u32>(n + 0x10);
                    out[v.blocks].size = sz;
                }
                v.bytes += sz;
                if (sz > v.largest)
                    v.largest = sz;
                if (sz == kVramPage)
                    v.pages++;
                v.blocks++;
                stack[sp++] = Rd<u32>(n + 0x08);
                stack[sp++] = Rd<u32>(n + 0x0C);
            }
            v.ok = true;
            return v;
        }

        // Reservation check (game thread, title), reported by the plugin thread.
        VramStat s_resUsedB = {}, s_resFreeB = {};
        u32      s_resBytes = 0, s_resPagesFit = 0;
        volatile int s_resVerdict = -1;      // -1 not run, 0 refused, 1 room
        u8       s_savedCrossBank = 0;

        // The game's VRAM allocator callback (0x10DACC) retries a failed
        // VRAM-B request in VRAM-A when this byte is set (0x10DB28; only
        // 0x402C08 sets it, 0 in every run seen). Cleared around our own
        // allocation so the right target can never land in VRAM-A (audit).
        constexpr u32 kVramCrossBank = 0x00633D94;

        u32 s_minMainFree = 0xFFFFFFFF, s_minDevFree = 0xFFFFFFFF;

        // ---- panic hooks: one line straight to SD from the dying thread ----
        constexpr u32 kFnSvcBreak     = 0x0013722C;     // svc 0x3C ; bx lr (abort / terminate path)
        constexpr u32 kInsnSvcBreak   = 0xEF00003C;
        constexpr u32 kFnFatalResult  = 0x0013B16C;     // (Result, caller pc): failed-result handler (ERR:F)
        constexpr u32 kInsnFatalRes   = 0xE1A02000;     // mov r2, r0
        hook::FunctionHook s_breakHook, s_fatalHook;
        volatile u32 s_panicLines = 0;
        char s_panicBuf[320];

        void PanicLine(const char *what, u32 a, u32 b)
        {
            if (__atomic_fetch_add(&s_panicLines, 1, __ATOMIC_ACQ_REL) >= 4)
                return;     // never flood the SD from a failing loop
            const HeapStat m = WalkHeap(MainHeapObj()), d = WalkHeap(DeviceHeapObj());
            constexpr double M = 1024.0 * 1024.0;
            snprintf(s_panicBuf, sizeof(s_panicBuf),
                     "PANIC %s %08lX %08lX | game main free %.2fM largest %.2fM of %.2fM%s, dev free %.2fM largest %.2fM of %.2fM%s",
                     what, a, b, m.freeBytes / M, m.largest / M, m.size / M, m.ok ? "" : "?",
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

    void EarlyBoot(FwkSettings &settings)
    {
        // Luma's 2 MiB block holds .text/.data/.bss (~0.82 MiB with the
        // 128 KiB block below), CTRPF's screen buffers (0.66 MiB) and only
        // ~0.51 MiB of newlib heap, which CTRPF's 1 MiB search heap alone
        // would overflow. Nothing has used System::Heap yet (InitHeap runs
        // right before PatchProcess and the pool is created on the first
        // allocation), so it gets the small static block: directory
        // listings (100 entries, 55 KiB) and line buffers still fit; the
        // search engine (0xB0000) is switched off. SD menu sounds would load
        // whole into the small heap: off too (audit).
        settings.AllowSearchEngine = false;
        settings.TryLoadSDSounds = false;
        s_earlyBootDone = true;
        s_ctrpfHeapOld = Heap::__ctrpf_heap_size;
        if (Heap::__ctrpf_heap == s_ctrpfSmallHeap)
        {
            // InitHeap got the static block (see operator new above)
            Heap::__ctrpf_heap_size = kCtrpfSmallHeap;
            s_ctrpfHeapShrunk = true;
        }
        else if (Heap::__ctrpf_heap && Heap::__ctrpf_heap_size > kCtrpfSmallHeap)
        {
            // A 1 MiB block was allocated after all (larger plugin block):
            // give it back and use the static one.
            ::operator delete(Heap::__ctrpf_heap);
            Heap::__ctrpf_heap = s_ctrpfSmallHeap;
            Heap::__ctrpf_heap_size = kCtrpfSmallHeap;
            s_ctrpfHeapShrunk = true;
        }
        s_ctrpfHeapNew = Heap::__ctrpf_heap_size;
    }

    bool PatchGameMemory()
    {
        // Runs before the game executes (PatchProcess), so nninitStartUp
        // sees the new size. The main heap gets 1 MiB less (it is computed
        // as the remainder). Not on a 2DS (no 3D screen) and not when the
        // player opted out of 3D with the SD file: the game keeps that MiB.
        u8 not2ds = 1;
        const bool is2ds = R_SUCCEEDED(CFGU_GetModelNintendo2DS(&not2ds)) && not2ds == 0;
        const bool optOut = File::Exists(kNo3dFile) == 1;
        if (is2ds || optOut)
        {
            s_noStereoReason = is2ds ? "off: 2DS (no 3D screen)" : "off: t3d_no3d.txt on SD";
            perf::LogNow(std::string("o3ds memory: ") + (is2ds ? "2DS" : "t3d_no3d.txt found") +
                         ": no right-eye buffers, game heaps left stock; " +
                         Utils::Format("CTRPF heap %luK -> %luK%s", s_ctrpfHeapOld / 1024, s_ctrpfHeapNew / 1024,
                                       s_ctrpfHeapShrunk ? "" : " (NOT shrunk)"));
            return false;
        }
        s_memPatched = mem::Replace32(kDevSizeInsnAddr, kDevSizeInsnOrig, kDevSizeInsnNew);
        if (!s_memPatched)
            s_noStereoReason = "off: memory patch failed";
#ifdef T3D_TEST_SIM_LUMA_CHARGE
        // EMULATOR TEST BUILDS ONLY (make o3ds T3D_EXTRA=-DT3D_TEST_SIM_LUMA_CHARGE).
        // Azahar does not charge the plugin block to the game; Luma does.
        // Take 2 MiB off the game's free-memory figure the way the console
        // does: replace the (dead on Old 3DS) New-3DS check in nninitStartUp
        // with "sub r6, r6, #0x200000".
        const bool sim = mem::Replace32(0x00108AAC, 0xEB002C44, 0xE2466602)
                      && mem::Replace32(0x00108AB0, 0xE3500000, mem::kInsnNop)
                      && mem::Replace32(0x00108AB4, 0x13A04649, mem::kInsnNop);
        perf::LogNow(std::string("TEST BUILD: simulated Luma 2 MiB charge ") + (sim ? "on" : "FAILED"));
#endif
        perf::LogNow(Utils::Format("o3ds memory: CTRPF heap %luK -> %luK%s, search engine off; game device heap %s",
                                   s_ctrpfHeapOld / 1024, s_ctrpfHeapNew / 1024, s_ctrpfHeapShrunk ? "" : " (NOT shrunk)",
                                   s_memPatched ? "+1 MiB (main heap -1 MiB) for the right-eye VRAM"
                                                : "patch FAILED: right-eye buffers will not be reserved"));
        return s_memPatched;
    }

    const char *NoStereoReason() { return s_noStereoReason; }

    bool VramRoomForRightTarget(u32 bytes)
    {
        // The allocator is first-fit by address (0x10FF70) with 0x80
        // alignment. Model that: the target takes the lowest-address free
        // block that holds it (+0x80 slack); then count the whole 1 MiB
        // pages every free block can still hold. Used pages plus those must
        // reach the 2 that VRAM-B holds with the flat buffers alone.
        VramBlock blk[kMaxFreeBlocks];
        s_resBytes = bytes;
        s_resUsedB = WalkVram(kVramAlloc + kVramUsedB);
        s_resFreeB = WalkVram(kVramAlloc + kVramFreeB, blk, kMaxFreeBlocks);
        s_resPagesFit = 0;
        bool room = s_resUsedB.ok && s_resFreeB.ok;
        if (room)
        {
            const u32 need = bytes + 0x80;
            int first = -1;
            for (int i = 0; i < static_cast<int>(s_resFreeB.blocks); i++)
                if (blk[i].size >= need && (first < 0 || blk[i].addr < blk[first].addr))
                    first = i;
            room = first >= 0;
            for (int i = 0; room && i < static_cast<int>(s_resFreeB.blocks); i++)
                s_resPagesFit += (i == first ? blk[i].size - need : blk[i].size) / kVramPage;
            room = room && s_resUsedB.pages + s_resPagesFit >= kVramPagesB;
        }
        __atomic_store_n(&s_resVerdict, room ? 1 : 0, __ATOMIC_RELEASE);
        return room;
    }

    void BeginRightTargetAlloc()
    {
        s_savedCrossBank = Rd<u8>(kVramCrossBank);
        *reinterpret_cast<volatile u8 *>(kVramCrossBank) = 0;
    }

    void EndRightTargetAlloc()
    {
        *reinterpret_cast<volatile u8 *>(kVramCrossBank) = s_savedCrossBank;
    }

    std::string VramReservationLine()
    {
        const int verdict = __atomic_load_n(&s_resVerdict, __ATOMIC_ACQUIRE);
        if (verdict < 0)
            return "";
        constexpr double K = 1024.0;
        return Utils::Format("o3ds vram at the right-target reservation: B used %.1fK (%lu pages)%s, free %.1fK (largest %.1fK)%s;"
                             " target %.1fK, pages that still fit %lu, cross-bank flag %u -> %s",
                             s_resUsedB.bytes / K, s_resUsedB.pages, s_resUsedB.ok ? "" : " [unreadable]",
                             s_resFreeB.bytes / K, s_resFreeB.largest / K, s_resFreeB.ok ? "" : " [unreadable]",
                             s_resBytes / K, s_resPagesFit, s_savedCrossBank,
                             verdict ? "room: per-layer depth available"
                                                         : "REFUSED (would displace a second atlas page): flat 3D only");
    }

    std::string VramLine()
    {
        const VramStat ua = WalkVram(kVramAlloc + kVramUsedA), ub = WalkVram(kVramAlloc + kVramUsedB);
        const VramStat fa = WalkVram(kVramAlloc + kVramFreeA), fb = WalkVram(kVramAlloc + kVramFreeB);
        constexpr double K = 1024.0;
        return Utils::Format("vram pages A %lu B %lu, free A %.1fK B %.1fK%s",
                             ua.pages, ub.pages, fa.bytes / K, fb.bytes / K,
                             ua.ok && ub.ok && fa.ok && fb.ok ? "" : " ?");
    }

    bool GameMemoryOk()
    {
        if (!s_memPatched)
            return false;
        const u32 dev = DeviceHeapObj();
        if (dev < 0x08000000 || dev >= 0x10000000 || Rd<u32>(dev + 0x18) != kExpMagic)
            return false;
        const u32 size = Rd<u32>(dev + 0x34) - Rd<u32>(dev + 0x30);
        return size + 0x1000 >= kDevSizeNew;
    }

    void InstallPanicHooks()
    {
        const hook::Result a = s_breakHook.Install(kFnSvcBreak, reinterpret_cast<void *>(OnSvcBreak), kInsnSvcBreak);
        const hook::Result b = s_fatalHook.Install(kFnFatalResult, reinterpret_cast<void *>(OnFatalResult), kInsnFatalRes);
        perf::LogNow(std::string("o3ds: panic log hooks: break ") + hook::ResultString(a) + ", fatal " + hook::ResultString(b));
    }

    bool Install()
    {
        if (s_installed)
            return true;
        s_bootTick = svcGetSystemTick();
        s_measureInstalled = s_measureHook.Install(kMeasureSlot, OnMeasureSlot, mem::kInsnNop) == hook::Result::Ok;
        s_installed = s_paceHook.Install(kPaceSlot, OnPaceSlot, mem::kInsnNop) == hook::Result::Ok;
        perf::LogNow(std::string("o3ds: frame pacing slot ") + (s_installed ? "installed" : "NOT installed (no 30 fps lock)") +
                     ", display measure slot " + (s_measureInstalled ? "installed" : "NOT installed"));
        return s_installed;
    }

    bool Installed() { return s_installed; }

    void SetLockMode(int m)
    {
        if (m < 0 || m >= kLockModeCount || m == s_mode)
            return;
        s_mode = m;
        perf::LogNow(std::string("30 fps lock mode: ") + LockModeName(m));
    }

    int LockMode() { return s_mode; }

    const char *LockModeName(int m)
    {
        switch (m)
        {
            case kLockAuto:   return "Auto (3D on, or in caves)";
            case kLockAlways: return "Always 30";
            case kLockOff:    return "Off (uncapped)";
        }
        return "?";
    }

    bool Engaged()      { return s_installed && s_engaged; }
    bool Underground()  { return s_underground; }
    const char *EngagedReason() { return ReasonName(s_engaged ? s_reason : 0); }

    void FlushEvents()
    {
        bool any = false;
        for (;;)
        {
            const u32 tail = s_evTail;
            if (tail == __atomic_load_n(&s_evHead, __ATOMIC_ACQUIRE))
                break;
            const Event e = s_ev[tail % kEvCount];
            __atomic_store_n(&s_evTail, tail + 1, __ATOMIC_RELEASE);
            const perf::Summary s = perf::Last();
            std::string what;
            switch (e.type)
            {
                case kEvLockOn:     what = std::string("30 fps lock ON (") + ReasonName(e.reason) + ")"; break;
                case kEvLockOff:    what = std::string("30 fps lock OFF (was ") + ReasonName(e.reason) + ")"; break;
                case kEvCaveIn:     what = "cave detector: underground"; break;
                case kEvCaveOut:    what = "cave detector: surface"; break;
                case kEvTimeout:    what = Utils::Format("pace wait timed out (vblanks %lu)", e.value); break;
                case kEvSuppressed: what = Utils::Format("(%lu lock on/off changes in the last 2 s not logged)", e.value); break;
                default:            what = "event ?"; break;
            }
            perf::Log(Utils::Format("event t=%.2fs ", e.sinceBootS) + what +
                      Utils::Format(" | last window fps %.1f max %.1fms", s.fpsAvg, s.frameMsMax));
            any = true;
        }
        if (any)
            perf::FlushLog();
    }

    std::string PaceLine()
    {
        const PaceStats p = s_pub[__atomic_load_n(&s_pubIdx, __ATOMIC_ACQUIRE)];
        char buf[176];
        snprintf(buf, sizeof(buf), "pace %s %s(%s) ug %d shown vb1 %lu vb2 %lu vb3 %lu vb4+ %lu (vb0 %lu) wait %.1fms eng %lu/%lu to %lu",
                 s_installed ? (s_measureInstalled ? "ok" : "ok-nomeasure") : "NOHOOK",
                 Engaged() ? "on" : "off", EngagedReason(), s_underground ? 1 : 0,
                 p.hist[1], p.hist[2], p.hist[3], p.hist[4], p.hist[0], p.waitMs,
                 p.engagedFrames, p.frames, p.timeouts);
        return buf;
    }

    std::string ShortLine()
    {
        const PaceStats p = s_pub[__atomic_load_n(&s_pubIdx, __ATOMIC_ACQUIRE)];
        char buf[96];
        snprintf(buf, sizeof(buf), "30 lock %s (%s) shown 60:%lu 30:%lu 20:%lu <20:%lu",
                 Engaged() ? "on" : "off", EngagedReason(), p.hist[1] + p.hist[0], p.hist[2], p.hist[3], p.hist[4]);
        return buf;
    }

    std::string MemLine()
    {
        const s64 appSize = osGetMemRegionSize(MEMREGION_APPLICATION);
        const s64 appUsed = osGetMemRegionUsed(MEMREGION_APPLICATION);
        const s64 sysSize = osGetMemRegionSize(MEMREGION_SYSTEM);
        const s64 sysUsed = osGetMemRegionUsed(MEMREGION_SYSTEM);
        const s64 appFree = appSize - appUsed, sysFree = sysSize - sysUsed;
        if (s_minAppFree < 0 || appFree < s_minAppFree) s_minAppFree = appFree;
        if (s_minSysFree < 0 || sysFree < s_minSysFree) s_minSysFree = sysFree;
        const struct mallinfo mi = mallinfo();
        const u32 heapCap = static_cast<u32>(fake_heap_end - fake_heap_start);
        constexpr double M = 1024.0 * 1024.0;
        char buf[360];
        int n = snprintf(buf, sizeof(buf),
                 "mem app free %.2fM (min %.2fM) of %.1fM, sys free %.2fM (min %.2fM) of %.1fM, plg heap used %.2fM of %.2fM, lin %.2fM",
                 appFree / M, s_minAppFree / M, appSize / M, sysFree / M, s_minSysFree / M, sysSize / M,
                 mi.uordblks / M, heapCap / M, __ctru_linear_heap_size / M);
        // The game's own heaps (what actually runs out): free, largest block,
        // size, and the minimum free seen at these samples.
        const HeapStat m = WalkHeap(MainHeapObj()), d = WalkHeap(DeviceHeapObj());
        if (m.size && n > 0 && n < static_cast<int>(sizeof(buf)))
        {
            if (m.ok && m.freeBytes < s_minMainFree) s_minMainFree = m.freeBytes;
            if (d.ok && d.freeBytes < s_minDevFree)  s_minDevFree = d.freeBytes;
            snprintf(buf + n, sizeof(buf) - n,
                     " | game main free %.2fM (min %.2fM) big %.2fM of %.2fM%s, dev free %.2fM (min %.2fM) big %.2fM of %.2fM%s",
                     m.freeBytes / M, s_minMainFree == 0xFFFFFFFF ? 0.0 : s_minMainFree / M, m.largest / M, m.size / M, m.ok ? "" : "?",
                     d.freeBytes / M, s_minDevFree == 0xFFFFFFFF ? 0.0 : s_minDevFree / M, d.largest / M, d.size / M, d.ok ? "" : "?");
        }
        return buf;
    }

    std::string LockStatus()
    {
        std::string s;
        switch (s_mode)
        {
            case kLockAuto:   s = "Auto"; break;
            case kLockAlways: s = "Always"; break;
            default:          s = "Off"; break;
        }
        if (!s_installed)
            return s + " (hook missing: unavailable)";
        if (Engaged())
            return s + " (now 30: " + EngagedReason() + ")";
        return s + " (now uncapped)";
    }
}

#endif // T3D_O3DS
