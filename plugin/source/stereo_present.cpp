#include "stereo_present.hpp"

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include <3ds/svc.h>    // svc.h has no extern "C" guard of its own (3ds.h provides it)
}

#include "game_layout.hpp"
#include "hooks.hpp"
#include "memory.hpp"
#include "stereo.hpp"
#include "stereo_layers.hpp"
#include "features.hpp"
#include "perf.hpp"
#include "ui_theme.hpp"

// How it works (see notes/display_present.md):
//
// The game renders the top screen into a 256x512 render target, copies it with
// a block-to-linear transfer into the top display buffer and swaps with
// nngxSwapBuffers(). That swap already supports NN_GX_DISPLAYMODE_STEREO
// (0x702): it reads the buffer bound to DISPLAY0_EXT, offsets it by the slot-2
// display env and sets the 3D bit for GSP. The game just never enables it.
//
// Per frame, from two free `nop` slots in the main loop:
//   kSlotAfterTopCopy  copy the same top render target into our right-eye
//                      buffer [cur idx], PAD rows in.
//   kSlotAfterBind     bind right buffer [new idx] to DISPLAY0_EXT, set the
//                      slot-2 row offset (= disparity) and the display mode.
//
// The right buffers are PAD rows taller on each side, so shifting the scanout
// window never reads outside them; the exposed edge is black.
//
// Top screen layout reminder: the 400 px screen-horizontal axis is the ROW axis
// of the 240x400 buffer, so a horizontal shift is a whole-row offset.

namespace t3d::stereo::present
{
    namespace
    {
        using namespace t3d::game;

        constexpr int kPadRows  = 16;
        constexpr int kWidth    = 240;      // pixels per row (screen-vertical)
        constexpr int kHeight   = 400;      // rows (screen-horizontal)
        constexpr int kBpp      = 3;        // RGB8
        constexpr int kRowBytes = kWidth * kBpp;
        constexpr int kBufRows  = kHeight + 2 * kPadRows;
        constexpr int kMaxDisparity = 24;   // per-layer limit; flat mode clamps to kPadRows

        using FnActiveDisplay  = void (*)(u32);
        using FnGenDisplaybufs = void (*)(s32, u32 *);
        using FnBindDisplaybuf = void (*)(u32);
        using FnDisplaybufStor = void (*)(u32, s32, s32, u32);
        using FnGetDisplayParam= void (*)(u32, s32 *);
        using FnCopyB2L        = void (*)(u32, u32, u32, u32, u32, u32, u32, u32, u32);

        const auto nngxActiveDisplay   = reinterpret_cast<FnActiveDisplay>(kFnNngxActiveDisplay);
        const auto nngxGenDisplaybuffers = reinterpret_cast<FnGenDisplaybufs>(kFnNngxGenDisplaybuffers);
        const auto nngxBindDisplaybuffer = reinterpret_cast<FnBindDisplaybuf>(kFnNngxBindDisplaybuffer);
        const auto nngxDisplaybufferStorage = reinterpret_cast<FnDisplaybufStor>(kFnNngxDisplaybufferStorage);
        const auto nngxGetDisplaybufferParameteri = reinterpret_cast<FnGetDisplayParam>(kFnNngxGetDisplaybufferParam);
        const auto copyResourceB2L     = reinterpret_cast<FnCopyB2L>(kFnCopyResourceB2L);

        struct DispEntry
        {
            u32 ids[2];
            u32 addrs[2];
            u32 idx;
            u32 w, h, fmt;
        };
        static_assert(sizeof(DispEntry) == 0x20, "display table entry layout");

        inline volatile DispEntry &TopDisp() { return *reinterpret_cast<volatile DispEntry *>(kDispTable); }
        inline volatile u32 &Reg(u32 addr)   { return *reinterpret_cast<volatile u32 *>(addr); }

        hook::SlotHook  s_preTopHook;
        hook::SlotHook  s_copyHook;
        hook::SlotHook  s_bindHook;

        State   s_state = State::NotInstalled;
        bool    s_enabled = true;
        bool    s_perLayer = true;          // stage 2, opt-in from the menu (falls back to stage 1)
        bool    s_invert = false;
        int     s_maxDisparity = 12;        // screen px at full slider (playtest: 8 too subtle)
        int     s_disparity = 0;            // applied this frame
        bool    s_frameStereo = false;      // right buffer written this frame

        // Right-eye display buffer sets. Several layouts exist so the "Stereo
        // test mode" menu can bisect the black top screen seen on real hardware
        // (v0.2.1/0.2.2; fine in Azahar). Each set is allocated lazily.
        struct RightSet
        {
            const char *name;
            u32 area;       // nngx storage area
            int padRows;    // extra rows on each side (room for the scanout shift)
            u32 ids[2];
            u32 addr[2];
            bool tried, ok;
        };
        RightSet s_sets[] = {
            { "VRAM-B, padded",   kNngxAreaVramB, kPadRows, {}, {}, false, false },
            { "same as left eye", 0,              0,        {}, {}, true,  true  },
            { "VRAM-B, no pad",   kNngxAreaVramB, 0,        {}, {}, false, false },
        };
        constexpr int kSetCount = sizeof(s_sets) / sizeof(s_sets[0]);
        constexpr int kSetSameAsLeft = 1;
        int     s_mode = 0;             // index into s_sets (menu)
        int     s_frameMode = 0;        // mode used for the frame in flight
        u32     s_lastRightShown = 0;   // debug: right address handed to the swap

        // Mode 0 set, for the status box
        u32    *s_rightAddr = s_sets[0].addr;
        bool    s_inFcram = false;

        bool CpuWritable(u32 addr, u32 len)
        {
            const u32 end = addr + len;
            return (addr >= 0x1F000000 && end <= 0x1F600000)      // VRAM
                || (addr >= 0x14000000 && end <= 0x1C000000)      // linear heap (old)
                || (addr >= 0x30000000 && end <= 0x40000000);     // linear heap (new)
        }

        void ClearToBlack(u32 addr, u32 len)
        {
            if (!CpuWritable(addr, len))
                return;
            std::memset(reinterpret_cast<void *>(addr), 0, len);
            svcFlushProcessDataCache(0xFFFF8001, addr, len);
        }

        // Runs on the game's main thread, inside the frame, with DISPLAY1 active.
        bool AllocateSet(RightSet &set)
        {
            if (set.tried)
                return set.ok;
            set.tried = true;
            const int rows = kHeight + 2 * set.padRows;

            nngxActiveDisplay(kNngxDisplay0Ext);
            nngxGenDisplaybuffers(2, set.ids);
            bool ok = true;
            for (int i = 0; i < 2 && ok; i++)
            {
                nngxBindDisplaybuffer(set.ids[i]);
                nngxDisplaybufferStorage(kNngxFmtRGB8, kWidth, rows, set.area);
                s32 addr = 0;
                nngxGetDisplaybufferParameteri(kNngxParamAddr, &addr);
                set.addr[i] = static_cast<u32>(addr);
                ok = addr != 0;
            }
            nngxActiveDisplay(kNngxDisplay1);

            if (ok)
            {
                ClearToBlack(set.addr[0], rows * kRowBytes);
                ClearToBlack(set.addr[1], rows * kRowBytes);
            }
            return set.ok = ok;
        }

        int ComputeDisparity()
        {
            int s = static_cast<int>(stereo_get_slider() * s_maxDisparity + 0.5f);
            if (s > kPadRows)
                s = kPadRows;
            // Default: right eye's image moves right -> picture sits behind the screen.
            // Scanout start row P - s shows image column 0 at screen column s.
            return s_invert ? s : -s;
        }

        // ---- slot hooks (game main thread) ----

        // Before BeginTop: decide whether this frame is stereo and, for
        // stage 2, prepare the right-eye render target.
        void OnPreTop()
        {
            perf::FrameStart();
            features::OnFrame();
            ui_theme::Tick();
            s_frameStereo = false;
            const float slider = stereo_get_slider();

            // Reserve the right-eye memory on the very first frame the display
            // is set up (title screen), whatever the slider says. On hardware,
            // VRAM is full of world textures by the time the player raises the
            // slider in-game, so a lazy allocation failed there (v0.2.3 debug:
            // "ALLOC FAILED") and the FCRAM fallback showed a black screen.
            if (s_state == State::WaitingForGame)
            {
                volatile DispEntry &top = TopDisp();
                if (top.ids[0] != 0 && top.w == kWidth && top.h == kHeight)
                {
                    s_state = State::Active;
                    if (!AllocateSet(s_sets[0]))
                        s_state = State::AllocFailed;
                    layers::ReserveRightTarget();
                }
            }

            s_frameMode = s_mode;
            const bool setOk = s_state != State::WaitingForGame && s_state != State::NotInstalled
                            && s_enabled && t3d::stereo::Enabled()
                            && AllocateSet(s_sets[s_frameMode]);
            s_frameStereo = setOk;
            layers::BeginFrame(s_frameStereo && s_perLayer,
                               slider * s_maxDisparity * (s_invert ? -1.0f : 1.0f));
        }

        void OnAfterTopCopy()
        {
            perf::TopDone();
            if (!s_frameStereo)
                return;

            if (s_frameMode == kSetSameAsLeft)
            {
                s_disparity = 0;    // the right eye scans out the left buffer itself
                return;
            }

            const RightSet &set = s_sets[s_frameMode];
            volatile DispEntry &top = TopDisp();
            const u32 idx = top.idx & 1;
            const u32 dst = set.addr[idx] + set.padRows * kRowBytes;

            // Stage 2: the right eye was rendered into its own target.
            // Stage 1 fallback: copy the left image and shift the scanout window.
            const u32 rightRes = layers::RightColorResourceThisFrame();
            const u32 srcRes = rightRes ? rightRes : Reg(kRenderGlobals + 0x18);

            // Same call the game makes for the left eye (fmt 1 = RGB8, no AA, no flip)
            copyResourceB2L(srcRes, 0, 0, dst, kWidth, kHeight, 1, 0, 0);

            s_disparity = (rightRes || set.padRows == 0) ? 0 : ComputeDisparity();
        }

        void OnAfterBind()
        {
            if (!s_frameStereo)
            {
                Reg(kNngxDisplayMode) = kModeNormal;
                return;
            }

            // The game just flipped top.idx and bound left buffer [idx]; mirror it.
            const u32 idx = TopDisp().idx & 1;
            const RightSet &set = s_sets[s_frameMode];
            const u32 id = s_frameMode == kSetSameAsLeft ? TopDisp().ids[idx] : set.ids[idx];
            nngxActiveDisplay(kNngxDisplay0Ext);
            nngxBindDisplaybuffer(id);
            nngxActiveDisplay(kNngxDisplay1);

            // swap: right = addr + bpp * (w * ((h - 400) - offY) + offX)
            //       => start row = 2P - offY; we want P + disparity.
            Reg(kNngxSlot2OffX) = 0;
            Reg(kNngxSlot2OffY) = static_cast<u32>(set.padRows - s_disparity);
            s_lastRightShown = (s_frameMode == kSetSameAsLeft ? TopDisp().addrs[idx] : set.addr[idx])
                             + (set.padRows + s_disparity) * kRowBytes;
            Reg(kNngxDisplayMode) = kModeStereo;
        }
    }

    bool Install()
    {
        if (s_state != State::NotInstalled)
            return true;

        // One extra transfer request per frame: give the command list headroom
        // (nngxCmdlistStorage(0x40000, 16) -> 32 requests).
        mem::Replace32(0x0010F334, 0xE3A01010 /*mov r1,#16*/, 0xE3A01020 /*mov r1,#32*/);

        // Stage 2 layer hooks; stage 1 still works if these refuse to install.
        layers::Install();

        if (s_preTopHook.Install(kSlotBeforeTop, OnPreTop, mem::kInsnNop) != hook::Result::Ok)
            return false;
        if (s_copyHook.Install(kSlotAfterTopCopy, OnAfterTopCopy, mem::kInsnNop) != hook::Result::Ok)
            return false;
        if (s_bindHook.Install(kSlotAfterBind, OnAfterBind, mem::kInsnNop) != hook::Result::Ok)
            return false;

        s_state = State::WaitingForGame;
        return true;
    }

    void SyncRightBuffersForMenu()
    {
        // The plugin menu pauses the game and flips between both framebuffer
        // pairs itself. Only the right buffer the game last presented holds a
        // current image, so the other pair showed a stale or black right eye
        // (the flicker found in playtest). Make both right buffers identical.
        if (Reg(kNngxDisplayMode) != kModeStereo)
            return;
        const RightSet &set = s_sets[s_frameMode];
        if (s_frameMode == kSetSameAsLeft || !set.ok)
            return;
        const u32 shown = TopDisp().idx & 1;
        const u32 len = (kHeight + 2 * set.padRows) * kRowBytes;
        const u32 src = set.addr[shown], dst = set.addr[shown ^ 1];
        if (!CpuWritable(src, len) || !CpuWritable(dst, len))
            return;
        std::memcpy(reinterpret_cast<void *>(dst), reinterpret_cast<const void *>(src), len);
        svcFlushProcessDataCache(0xFFFF8001, dst, len);
    }

    int  TestModeCount()            { return kSetCount; }
    const char *TestModeName(int m) { return (m >= 0 && m < kSetCount) ? s_sets[m].name : "?"; }
    void SetTestMode(int m)         { if (m >= 0 && m < kSetCount) s_mode = m; }
    int  TestMode()                 { return s_mode; }

    std::string DebugText()
    {
        const RightSet &set = s_sets[s_mode];
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "mode %d: %s (%s)\nleft  %08lX %08lX idx %lu\nright %08lX %08lX\nshown %08lX  nngx mode %03lX  disp %d",
                 s_mode, set.name, set.ok ? "ok" : (set.tried ? "ALLOC FAILED" : "not yet"),
                 TopDisp().addrs[0], TopDisp().addrs[1], TopDisp().idx & 1,
                 set.addr[0], set.addr[1], s_lastRightShown,
                 static_cast<u32>(Reg(kNngxDisplayMode)), s_disparity);
        return buf;
    }

    State GetState() { return s_state; }

    const char *StateString(State s)
    {
        switch (s)
        {
            case State::NotInstalled:   return "not installed";
            case State::WaitingForGame: return "waiting (raise the 3D slider)";
            case State::Active:         return "active";
            case State::AllocFailed:    return "right-eye buffer allocation failed";
        }
        return "?";
    }

    void SetEnabled(bool on)        { s_enabled = on; }
    bool Enabled()                  { return s_enabled; }
    void SetMaxDisparity(int px)    { s_maxDisparity = px < 0 ? 0 : (px > kMaxDisparity ? kMaxDisparity : px); }
    int  MaxDisparity()             { return s_maxDisparity; }
    void SetPerLayer(bool on)       { s_perLayer = on; }
    bool PerLayer()                 { return s_perLayer; }
    void SetInvert(bool invert)     { s_invert = invert; }
    bool Inverted()                 { return s_invert; }
    int  CurrentDisparity()         { return s_frameStereo ? s_disparity : 0; }
    u32  RightBufferAddr(int idx)   { return s_rightAddr[idx & 1]; }
    bool RightBuffersInFcram()      { return s_inFcram; }
}
