#include "perf_probes.hpp"

extern "C" {
#include <3ds/svc.h>    // no extern "C" guard of its own
}

#include "hooks.hpp"
#include "memory.hpp"
#include "perf.hpp"
#include "perf_fixes.hpp"

#include <utility>

// Where a frame goes (see notes/performance.md). The game's main loop
// (FUN_0010709c) per frame:
//
//   GameState::Update (0x50F554): fixed 1/60 s accumulator; runs Main tick
//       0x3DB764 once per 1/60 s of elapsed time (2x after a 33 ms frame),
//       then 0x3D26D8 -> camera + lighting update 0x484558 (once per frame).
//   wait for the VBlank callback to kick the previous command list
//       (nngxWaitVSync 0x10ECD4 in a loop)
//   top draw  (perf "top")      bottom draw (Sec::Bottom)
//   nngxSplitDrawCmdlist, nngxWaitCmdlistDone 0x10EC38 (GPU still busy with
//       the previous frame), nngxSwapBuffers
//
// Lighting: the light map is computed by 0x488000 in a "LightingThread" job
// (vtable 0x5D7148) every third frame; 0x484558 sleeps in 1 ms steps until the
// job is done (0x484BE8). The job's core id tells whether it really runs in
// parallel (New 3DS core 2?) or steals time from the main thread.

namespace t3d::perf::probes
{
    namespace
    {
        constexpr u32 kFnMainTick        = 0x003DB764; // (Main *, float dt)   push {r4-r8,lr}
        constexpr u32 kFnLightingUpdate  = 0x00484558; // (Lighting *, Main *) push {r4-fp,lr}
        // LightingThread job vtable 0x5D7148, slot 2 = Run (0x21A6E8 -> 0x488000).
        // Patched as a plain function pointer: a CTRPF MITM hook on a worker
        // thread would depend on that thread's TLS slots (CheckTlsValue).
        constexpr u32 kJobRunSlot        = 0x005D7150;
        constexpr u32 kFnJobRun          = 0x0021A6E8;
        constexpr u32 kFnWaitCmdlistDone = 0x0010EC38; // ()                   push {r4-r6,lr}
        constexpr u32 kFnWaitVSync       = 0x0010ECD4; // (u32 display)        push {r4-r8,lr}

        hook::FunctionHook s_tickHook, s_lightHook, s_gpuHook, s_vsyncHook;
        int  s_installed = 0;

        u32  s_mainThreadId = 0;
        volatile u32 s_lastJobEndLo = 0;    // low word of the tick the last job finished
        volatile bool s_jobOnMain = false;  // last job ran on the main thread

        u32 CurrentThreadId()
        {
            u32 id = 0;
            svcGetThreadId(&id, 0xFFFF8000 /*CUR_THREAD_HANDLE*/);
            return id;
        }

        void Hooked_MainTick(u32 self, float dt)
        {
            const u64 t0 = svcGetSystemTick();
            if (!s_mainThreadId)
                s_mainThreadId = CurrentThreadId();
            hook::FunctionHook::CallOriginal<void>(self, dt);
            AddSection(Sec::Tick, svcGetSystemTick() - t0);
            CountTick();
        }

        void Hooked_LightingUpdate(u32 lighting, u32 main, u32 a2, u32 a3)
        {
            const u64 t0 = svcGetSystemTick();
            s_jobOnMain = false;
            hook::FunctionHook::CallOriginal<void>(lighting, main, a2, a3);
            const u64 t1 = svcGetSystemTick();
            AddSection(Sec::Light, t1 - t0);
            // The wait loop ends right after the worker job finishes: if a job
            // finished inside this call on another thread, we waited for it.
            const u32 end = s_lastJobEndLo;
            const u32 lo0 = static_cast<u32>(t0), span = static_cast<u32>(t1 - t0);
            if (!s_jobOnMain && end - lo0 <= span)
                AddSection(Sec::LightWait, end - lo0);
        }

        u32 JobRun(u32 job)
        {
            const u64 t0 = svcGetSystemTick();
            const u32 r = reinterpret_cast<u32 (*)(u32)>(kFnJobRun)(job);
            const u64 t1 = svcGetSystemTick();
            const bool onMain = s_mainThreadId && CurrentThreadId() == s_mainThreadId;
            if (onMain)
                s_jobOnMain = true;
            s_lastJobEndLo = static_cast<u32>(t1);
            NoteLightJob(t1 - t0, svcGetProcessorID() | (onMain ? 0x10 : 0));
            return r;
        }

        void Hooked_WaitCmdlistDone(u32 a0, u32 a1, u32 a2, u32 a3)
        {
            const u64 t0 = svcGetSystemTick();
            // Bottom screen = from the top copy to here (main loop only).
            if (const u64 top = TopDoneTick())
            {
                if (t0 > top)
                    AddSection(Sec::Bottom, t0 - top);
                ClearTopDoneTick();
            }
            hook::FunctionHook::CallOriginal<void>(a0, a1, a2, a3);
            AddSection(Sec::GpuWait, svcGetSystemTick() - t0);
        }

        void Hooked_WaitVSync(u32 display, u32 a1, u32 a2, u32 a3)
        {
            const u64 t0 = svcGetSystemTick();
            hook::FunctionHook::CallOriginal<void>(display, a1, a2, a3);
            AddSection(Sec::VsyncWait, svcGetSystemTick() - t0);
        }

        // ---- top-screen draw layers: one timing hook per function ----
        struct DrawProbe { u32 addr; u32 insn; Sec sec; };
        constexpr DrawProbe kDraw[] = {
            { 0x0029D304, 0xE92D4010, Sec::DrawGameplay },
            { 0x0017F418, 0xE2800A01, Sec::DrawSky },
            { 0x0051E0E8, 0xE92D4FF0, Sec::DrawLightMap },
            { 0x001DD048, 0xE92D4FF7, Sec::DrawUgBg },
            { 0x0051D0F4, 0xE92D4FF1, Sec::DrawBlack },
            { 0x00521C3C, 0xE92D4FF0, Sec::DrawWalls },
            { 0x00521E40, 0xE92D4FF0, Sec::DrawWalls },
            { 0x00520F94, 0xE92D4FF0, Sec::DrawNonSolid },
            { 0x0051894C, 0xE92D4FF0, Sec::DrawExtras },
            { 0x0051E43C, 0xE92D4FF0, Sec::DrawExtras },
            { 0x0051B2E4, 0xE92D5FF0, Sec::DrawTiles },
            { 0x0051A71C, 0xE92D4FF1, Sec::DrawLiquid },
            { 0x005221F0, 0xE92D4FF1, Sec::DrawLiquid },
            { 0x0045E548, 0xE92D4FF0, Sec::DrawWaterfall },
            { 0x0045EA60, 0xE92D4070, Sec::DrawWaterfall },
            { 0x0045E4D0, 0xE92D4070, Sec::DrawWaterfall },
            { 0x00376BC4, 0xE92D4FF3, Sec::DrawNpc },
            { 0x00517F80, 0xE92D4FF0, Sec::DrawPlayer },
            { 0x004824CC, 0xE92D4FF3, Sec::DrawDust },
            { 0x003AA4F0, 0xE92D4FF0, Sec::DrawDust },
            { 0x00496F30, 0xE92D4FF0, Sec::DrawDust },
            { 0x003C8D78, 0xE92D4FF0, Sec::DrawItems },
            { 0x00297C84, 0xE92D4FF0, Sec::DrawOverlays },
        };
        constexpr int kDrawCount = sizeof(kDraw) / sizeof(kDraw[0]);
        hook::FunctionHook s_drawHooks[kDrawCount];

        // None of these take stack arguments (checked: no [sp,#>=frame] loads)
        // and the body below uses no VFP registers, so s0.. pass through.
        // The backdrop pass (0x1DD048) also brackets perf_fixes' run merge,
        // which must flush its last pending run before the pass returns.
        // 0x1DD048 sets every VFP register it reads before using it, and
        // BackdropPassBegin only does integer stores.
        constexpr u32 kFnUgBg = 0x001DD048;

        template <int I>
        void DrawProbeCb(u32 a0, u32 a1, u32 a2, u32 a3)
        {
            const u64 t0 = svcGetSystemTick();
            if constexpr (kDraw[I].addr == kFnUgBg)
                t3d::perf_fixes::BackdropPassBegin();
            hook::FunctionHook::CallOriginal<void>(a0, a1, a2, a3);
            if constexpr (kDraw[I].addr == kFnUgBg)
                t3d::perf_fixes::BackdropPassEnd();
            AddSection(kDraw[I].sec, svcGetSystemTick() - t0);
        }

        template <int... I>
        void *const *DrawCallbacks(std::integer_sequence<int, I...>)
        {
            static void *const table[] = { reinterpret_cast<void *>(DrawProbeCb<I>)... };
            return table;
        }

        bool Try(hook::FunctionHook &h, u32 target, void *cb, u32 insn)
        {
            if (h.Install(target, cb, insn) != hook::Result::Ok)
                return false;
            s_installed++;
            return true;
        }
    }

    void Install()
    {
        Try(s_tickHook,  kFnMainTick,        reinterpret_cast<void *>(Hooked_MainTick),        0xE92D41F0);
        Try(s_lightHook, kFnLightingUpdate,  reinterpret_cast<void *>(Hooked_LightingUpdate),  0xE92D4FF0);
        void *const *cbs = DrawCallbacks(std::make_integer_sequence<int, kDrawCount>{});
        for (int i = 0; i < kDrawCount; i++)
            if (Try(s_drawHooks[i], kDraw[i].addr, cbs[i], kDraw[i].insn) && kDraw[i].addr == kFnUgBg)
                t3d::perf_fixes::SetPassHooked(true);
        if (mem::Replace32(kJobRunSlot, kFnJobRun, reinterpret_cast<u32>(JobRun)))
            s_installed++;
        Try(s_gpuHook,   kFnWaitCmdlistDone, reinterpret_cast<void *>(Hooked_WaitCmdlistDone), 0xE92D4070);
        Try(s_vsyncHook, kFnWaitVSync,       reinterpret_cast<void *>(Hooked_WaitVSync),       0xE92D41F0);
    }

    int InstalledCount() { return s_installed; }
}
