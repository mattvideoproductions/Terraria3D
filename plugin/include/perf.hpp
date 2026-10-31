#pragma once
// Frame timing telemetry measured on the game's main thread, plus an SD-card
// log written from the plugin thread. Built so hardware playtests produce data
// instead of videos (stock vs stage 1 vs per-layer cost).
#include <3ds/types.h>
#include <string>

namespace t3d::perf
{
    // Game main thread (main-loop slot hooks)
    void    FrameStart();               // before the top screen is drawn
    void    TopDone();                  // after the top screen's copy is queued
    void    AddReplayTicks(u64 ticks);  // stage-2 right-eye replay CPU time

    // Frame breakdown sections (perf_probes.cpp). Ticks summed per window and
    // reported as the per-frame average.
    enum class Sec : u8
    {
        Tick,       // game logic ticks (Main::Update, 1/60 s each)
        Light,      // per-frame lighting update on the main thread (incl. wait)
        LightWait,  // main thread waiting for the lighting worker job
        Bottom,     // bottom-screen draw (between top copy and the GPU wait)
        GpuWait,    // nngxWaitCmdlistDone: previous frame's GPU work still running
        VsyncWait,  // nngxWaitVSync: idle until the next VBlank
        // Top-screen draw breakdown (world layers, notes/draw_order.md)
        DrawGameplay, // 0x29D304: sky + world + name tags (rest of "top" = HUD/UI + flush)
        DrawSky,    // 0x17F418 sky / surface backgrounds
        DrawLightMap, // 0x51E0E8 per-tile light / map reveal
        DrawUgBg,   // 0x1DD048 underground backdrop
        DrawBlack,  // 0x51D0F4 unlit-tile pass
        DrawWalls,  // 0x521C3C + 0x521E40
        DrawNonSolid, // 0x520F94
        DrawExtras, // 0x51894C + 0x51E43C (wires, decorations)
        DrawTiles,  // 0x51B2E4 solid tiles
        DrawLiquid, // 0x51A71C + 0x5221F0
        DrawWaterfall, // 0x45E548 + 0x45EA60 + 0x45E4D0
        DrawNpc,    // 0x376BC4
        DrawPlayer, // 0x517F80
        DrawDust,   // 0x4824CC + 0x3AA4F0 (gore) + 0x496F30 (rain)
        DrawItems,  // 0x3C8D78
        DrawOverlays, // 0x297C84 name tags, chat bubbles
        Count
    };
    void    AddSection(Sec s, u64 ticks);
    void    CountTick();                // one logic tick ran
    void    NoteLightJob(u64 ticks, int core); // lighting job (any thread)
    u64     TopDoneTick();              // tick of the last TopDone() (0 = consumed)
    void    ClearTopDoneTick();

    // Last completed window (about 1 s)
    struct Summary
    {
        float   fpsAvg = 0, fpsMin = 0;
        float   topMs = 0, replayMs = 0, frameMsMax = 0;
        u32     cmdPeak = 0;                // peak sampled GPU command-list bytes (game guard: 0x3F000)
        u32     frames = 0;
        u32     seq = 0;                // increments every window
        float   secMs[static_cast<int>(Sec::Count)] = {};
        float   ticksPerFrame = 0;
        float   lightJobMs = 0;             // average per lighting job
        u32     lightJobs = 0;              // jobs finished in the window
        int     lightCore = -1;             // CPU core of the last lighting job
    };
    Summary Last();
    std::string SummaryLine();          // "fps 58.9 (min 41.2) top 9.1ms replay 0.8ms"
    std::string DetailLine();
    std::string DrawLine();             // per-layer top-screen draw times           // "upd 3.1x1.0 lit 0.4/w0.0 job 4.0c1x20 bot 1.2 gpu 0.0 vs 2.0"

    // Plugin thread: appends to /luma/plugins/000400000016A900/t3d_log.txt
    void    Log(const std::string &line);    // event line (timestamped)
    void    LogWindowIfNew(const std::string &context); // one line per window
    const char *LogPath();
#ifndef T3D_O3DS
    // Any thread, panic paths only: this line, written now.
    void    PanicWrite(const char *line);
#endif
#ifdef T3D_O3DS
    // Old 3DS build: the log is batched (one SD write per ~5 s); LogNow
    // writes at once (events, settings, failures). Plugin thread only.
    void    LogNow(const std::string &line);
    void    FlushLog();
    // Any thread, panic paths only: pending batch + this line, written now.
    void    PanicWrite(const char *line);
    // Game thread: the pacing wait ran between the top copy and the GPU
    // wait; move the "bottom screen" start past it.
    void    ShiftTopDoneTick(u64 ticks);
    float   LastFlushMs();
    // "frame 33.4ms avg 41.0 max" (last window)
    std::string FrameLine();
#endif
}
