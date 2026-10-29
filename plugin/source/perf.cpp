#include "perf.hpp"

#include <CTRPluginFramework.hpp>
extern "C" {
#include <3ds/svc.h>    // no extern "C" guard of its own
}
#include <cstdio>

using namespace CTRPluginFramework;

namespace t3d::perf
{
    namespace
    {
        constexpr double kTicksPerMs = 268111.856;   // SYSCLOCK_ARM11 / 1000
        constexpr u32    kWindowFrames = 60;
        constexpr const char *kLogPath = "/luma/plugins/000400000016A900/t3d_log.txt";

        // Accumulators (game thread only)
        u64 s_frameStart = 0, s_lastFrameStart = 0;
        u64 s_sumFrame = 0, s_maxFrame = 0, s_sumTop = 0, s_sumReplay = 0;
        u32 s_frames = 0;
        u32 s_cmdPeak = 0;
        u64 s_topDoneTick = 0;

        // Frame breakdown (perf_probes.cpp)
        constexpr int kSecCount = static_cast<int>(Sec::Count);
        u64 s_sec[kSecCount] = {};
        u32 s_ticks = 0;
        // Lighting job: may run on a worker thread, so only plain word stores
        volatile u32 s_jobTicksLo = 0, s_jobCount = 0;
        volatile int s_jobCore = -1;
        u32 s_jobTicksSeen = 0, s_jobCountSeen = 0;

        // Game sprite batch; it samples the bound command list's used bytes at +0x7C
        constexpr u32 kSpriteBatchPtr = 0x0086D278;

        // Published window (read by the plugin thread; a torn read only
        // garbles one log line, never memory safety)
        volatile Summary s_last;
        u32 s_loggedSeq = 0;

        u64 s_bootTick = 0;

        void Append(const std::string &line)
        {
            File f;
            if (File::Open(f, kLogPath, File::WRITE | File::CREATE | File::APPEND) != File::SUCCESS)
                return;
            f.WriteLine(line);
            f.Close();
        }

        float Seconds()
        {
            if (!s_bootTick)
                s_bootTick = svcGetSystemTick();
            return static_cast<float>((svcGetSystemTick() - s_bootTick) / (kTicksPerMs * 1000.0));
        }
    }

    void FrameStart()
    {
        const u64 now = svcGetSystemTick();
        if (s_lastFrameStart)
        {
            const u64 dt = now - s_lastFrameStart;
            s_sumFrame += dt;
            if (dt > s_maxFrame)
                s_maxFrame = dt;
            if (++s_frames >= kWindowFrames)
            {
                Summary sum;
                const double avgMs = (s_sumFrame / kTicksPerMs) / s_frames;
                sum.fpsAvg = avgMs > 0 ? static_cast<float>(1000.0 / avgMs) : 0;
                sum.frameMsMax = static_cast<float>(s_maxFrame / kTicksPerMs);
                sum.fpsMin = sum.frameMsMax > 0 ? 1000.0f / sum.frameMsMax : 0;
                sum.topMs = static_cast<float>((s_sumTop / kTicksPerMs) / s_frames);
                sum.replayMs = static_cast<float>((s_sumReplay / kTicksPerMs) / s_frames);
                sum.frames = s_frames;
                sum.cmdPeak = s_cmdPeak;
                s_cmdPeak = 0;
                for (int i = 0; i < kSecCount; i++)
                {
                    sum.secMs[i] = static_cast<float>((s_sec[i] / kTicksPerMs) / s_frames);
                    s_sec[i] = 0;
                }
                sum.ticksPerFrame = static_cast<float>(s_ticks) / s_frames;
                s_ticks = 0;
                const u32 jt = s_jobTicksLo, jc = s_jobCount;
                sum.lightJobs = jc - s_jobCountSeen;
                sum.lightJobMs = sum.lightJobs
                    ? static_cast<float>(((jt - s_jobTicksSeen) / kTicksPerMs) / sum.lightJobs) : 0;
                sum.lightCore = s_jobCore;
                s_jobTicksSeen = jt;
                s_jobCountSeen = jc;
                sum.seq = s_last.seq + 1;
                const_cast<Summary &>(s_last) = sum;
                s_sumFrame = s_maxFrame = s_sumTop = s_sumReplay = 0;
                s_frames = 0;
            }
        }
        s_lastFrameStart = now;
        s_frameStart = now;
    }

    void TopDone()
    {
        s_topDoneTick = svcGetSystemTick();
        if (s_frameStart)
            s_sumTop += s_topDoneTick - s_frameStart;
        // The batch samples the command list's used bytes while flushing; the
        // sample is reset at VBlank, so read it right after the top screen.
        if (const u32 batch = *reinterpret_cast<volatile u32 *>(kSpriteBatchPtr))
        {
            const u32 bytes = *reinterpret_cast<volatile u32 *>(batch + 0x7C);
            if (bytes > s_cmdPeak && bytes < 0x01000000)
                s_cmdPeak = bytes;
        }
    }

    void AddReplayTicks(u64 ticks) { s_sumReplay += ticks; }

    void AddSection(Sec s, u64 ticks)
    {
        const int i = static_cast<int>(s);
        if (i >= 0 && i < kSecCount)
            s_sec[i] += ticks;
    }

    void CountTick() { s_ticks++; }

    void NoteLightJob(u64 ticks, int core)
    {
        s_jobTicksLo = s_jobTicksLo + static_cast<u32>(ticks);  // wraps; deltas stay right
        s_jobCore = core;
        s_jobCount = s_jobCount + 1;
    }

    u64  TopDoneTick()      { return s_topDoneTick; }
    void ClearTopDoneTick() { s_topDoneTick = 0; }

    Summary Last() { return const_cast<const Summary &>(s_last); }

    std::string SummaryLine()
    {
        const Summary s = Last();
        char buf[128];
        snprintf(buf, sizeof(buf), "fps %.1f (min %.1f) top %.1fms 3D %.1fms cmd %lu%%",
                 s.fpsAvg, s.fpsMin, s.topMs, s.replayMs, (s.cmdPeak * 100) / 0x3F000);
        return buf;
    }

    std::string DetailLine()
    {
        const Summary s = Last();
        auto ms = [&](Sec sec) { return s.secMs[static_cast<int>(sec)]; };
        char buf[160];
        snprintf(buf, sizeof(buf), "upd %.1fms x%.2f lit %.1f/w%.1f job %.1fms c%d n%lu bot %.1f gpu %.1f vs %.1f",
                 ms(Sec::Tick), s.ticksPerFrame, ms(Sec::Light), ms(Sec::LightWait),
                 s.lightJobMs, s.lightCore, s.lightJobs,
                 ms(Sec::Bottom), ms(Sec::GpuWait), ms(Sec::VsyncWait));
        return buf;
    }

    std::string DrawLine()
    {
        const Summary s = Last();
        static const char *const kNames[] = {
            "gp", "sky", "lmap", "ugbg", "blk", "wall", "nsol", "ext", "tile",
            "liq", "wfall", "npc", "plr", "dust", "item", "ovl" };
        std::string out = "draw";
        char buf[24];
        const int first = static_cast<int>(Sec::DrawGameplay);
        for (int i = first; i < kSecCount && i - first < static_cast<int>(sizeof(kNames) / sizeof(kNames[0])); i++)
        {
            snprintf(buf, sizeof(buf), " %s %.1f", kNames[i - first], s.secMs[i]);
            out += buf;
        }
        return out;
    }

    void Log(const std::string &line)
    {
        char ts[24];
        snprintf(ts, sizeof(ts), "[%8.2f] ", Seconds());
        Append(ts + line);
    }

    void LogWindowIfNew(const std::string &context)
    {
        const Summary s = Last();
        if (s.seq == 0 || s.seq == s_loggedSeq)
            return;
        s_loggedSeq = s.seq;
        Log(SummaryLine() + " | " + DetailLine() + " | " + DrawLine() + " | " + context);
    }

    const char *LogPath() { return kLogPath; }
}
