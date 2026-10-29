#include "stereo_layers.hpp"

#include "game_layout.hpp"
#include "hooks.hpp"
#include "memory.hpp"
#include "perf.hpp"

extern "C" {
#include <3ds/svc.h>
}

using namespace t3d::game;

// ---------------------------------------------------------------------------
// Game structures (v5.6.0, notes/stage2_stereo.md section E1)
// ---------------------------------------------------------------------------
namespace
{
    // SpriteBatch field offsets
    constexpr u32 kSbCapacity   = 0x08;   // sprites per chunk
    constexpr u32 kSbRecords    = 0x10;   // Record ** (current segment, index from 0)
    constexpr u32 kSbPending    = 0x28;   // sprites pending flush
    constexpr u32 kSbVertexBuf  = 0x40;   // VB object; +0x08 = size in bytes / 4 -> slots
    constexpr u32 kSbLayer      = 0x6C;   // u16 layer counter (write-only in game code)
    constexpr u32 kSbScreenMgr  = 0x70;
    constexpr u32 kSbFlushed    = 0x74;   // sprites flushed this frame (index offset)
    constexpr u32 kSbSegCounter = 0x78;   // guard sampling counter (mod 10)
    constexpr u32 kSbCmdBytes   = 0x7C;   // sampled command-list bytes
    constexpr u32 kRecLayer     = 0x24;   // u16 stamp copied from +0x6C at submit

    // Render globals (kRenderGlobals = 0x633878)
    constexpr u32 kGdDepthStencil = kRenderGlobals + 0x14;
    constexpr u32 kGdTopRT        = kRenderGlobals + 0x1C;
    constexpr u32 kGdProgram      = kRenderGlobals + 0x30;
    constexpr u32 kGdLocProjection= kRenderGlobals + 0x3C;
    constexpr u32 kGdCurrentRT    = 0x008C888C;  // nn::gd bound render target (shadow)

    // Patch sites
    constexpr u32 kFnLayerInc = 0x001D8BF8;
    constexpr u32 kFnLayerDec = 0x001D8BE8;
    constexpr u32 kFnEnd      = 0x0010FAFC;
    constexpr u32 kCmdlistSizeInsn  = 0x0010F338; // mov r0,#0x40000
    constexpr u32 kCmdlistGuardInsn = 0x0010FC8C; // cmp r0,#0x3F000

    // Game functions
    using FnSetRT        = u32 (*)(u32 rt);
    using FnSetUniform   = u32 (*)(u32 program, u32 location, const float *m);
    using FnClear        = u32 (*)(u32 rt, u32 ds, const u32 *rgba, float depth);
    using FnCreateRes    = u32 (*)(const u32 *desc, u32, u32, u32 *outRes, u32);
    using FnCreateRT     = u32 (*)(u32 res, const u32 *mip, u32 *outRT);
    using FnLock         = void (*)(u32 screenMgr);
    using FnBindTexture  = void (*)(u32 screenMgr, u32 source, u32);
    using FnDrawIndexed  = void (*)(u32 indexCount, u32 indexOffset);
    using FnCmdlistParam = void (*)(u32 pname, u32 *out);

    const auto gdSetRenderTarget   = reinterpret_cast<FnSetRT>(0x0011AE48);
    const auto gdSetFloatUniform   = reinterpret_cast<FnSetUniform>(0x0011B24C);
    const auto gdClearTargets      = reinterpret_cast<FnClear>(0x00112D18);
    const auto gdCreateTexResource = reinterpret_cast<FnCreateRes>(0x001134E8);
    const auto gdCreateRenderTarget= reinterpret_cast<FnCreateRT>(0x00111F78);
    const auto screenLock          = reinterpret_cast<FnLock>(0x00156F3C);
    const auto screenUnlock        = reinterpret_cast<FnLock>(0x00156F48);
    const auto bindSpriteTexture   = reinterpret_cast<FnBindTexture>(0x00119DBC);
    const auto drawIndexed         = reinterpret_cast<FnDrawIndexed>(0x00124C98);
    const auto nngxGetCmdlistParam = reinterpret_cast<FnCmdlistParam>(0x0012907C);

    inline u32 &R32(u32 addr) { return *reinterpret_cast<u32 *>(addr); }
    inline s32 &S32(u32 addr) { return *reinterpret_cast<s32 *>(addr); }

    // -----------------------------------------------------------------------
    // Layer table: LR of each SpriteBatch::NextLayer call (call site + 4) in
    // the background pass 0x17F418 and world pass 0x522648 -> depth.
    // Depth: +1 far behind the screen ... 0 screen plane ... negative pops out.
    // Index 0 is reserved for "segment start" (stamp 0, nothing called yet).
    // -----------------------------------------------------------------------
    struct LayerSite { u32 lr; float depth; };
    // Depth design (v0.3.2). Units: fraction of "Depth strength" (default 12 px).
    //  * The play plane (ground, player, NPCs) sits on the screen surface.
    //  * Depth never increases along the draw order where things overlap, so
    //    occlusion and disparity agree (a later layer is never "behind" an
    //    earlier one it covers).
    //  * Backgrounds get a graded parallax; small, short-lived things (dust,
    //    gore, damage numbers, projectiles) pop out for fun, scaled by the
    //    "Pop-out effects" menu setting. Persistent UI only floats a little.
    // Pass names were corrected by an in-emulator probe (each pass given a
    // unique shift, objects measured): 0x522738 draws trees/plants/furniture,
    // 0x5227CC draws the solid ground.
    constexpr LayerSite kSites[] = {
        // background 0x17F418
        { 0x0017F45C, 1.00f },  // B1 sun / moon / stars: farthest
        { 0x0017F46C, 0.85f },  // B2 far surface backgrounds (mountains) [probe]
        { 0x0017F518, 0.80f },  // B3 far layer (clouds)
        { 0x0017F530, 0.72f },  // B4 biome sets fading in
        { 0x0017F548, 0.64f },  // B5 biome sets, y anchor +350
        { 0x0017F560, 0.55f },  // B6 nearest hills, y anchor +400
        // world 0x522648
        { 0x0052267C, 0.45f },  // W0 underground / cavern backdrop
        { 0x005226AC, 0.30f },  // W1 back waterfalls (+ build preview)
        { 0x005226CC, 0.30f },  // W2 walls
        { 0x005226E4, 0.30f },  // W3 wall overlay
        { 0x005226FC, 0.24f },  // W4 (unidentified, between walls and trees)
        { 0x00522714, 0.22f },  // W5 tile extras
        { 0x00522738, 0.18f },  // W6 trees, plants, torches, furniture, signs [probe]: just behind the player
        { 0x00522750, 0.18f },  // W7 waterfall tile pass
        { 0x00522768, 0.00f },  // W8 solid tiles / liquids / NPCs-behind
        { 0x00522794, 0.00f },  // W8b
        { 0x005227CC, 0.00f },  // W8b' solid ground and water [probe]
        { 0x005227E0, 0.00f },  // W9 NPCs + deferred sprites
        { 0x00522858, -0.12f }, // W10 projectiles: pop
        { 0x0052289C, -0.04f }, // W11 players: a hair in front of the ground [probe]
        { 0x005228B4, -0.04f }, // W12 dropped items
        { 0x005228CC, 0.05f },  // W13 rain: just behind the play plane (playtest)
        { 0x005228EC, -0.25f }, // W14 gore: flying chunks pop out
        { 0x00522904, -0.40f }, // W15 dust / particles: pop out (playtest: "fun")
        { 0x0052291C, 0.00f },  // W16 front waterfalls
        { 0x00522954, 0.00f },  // W17 liquid front / overlay
        { 0x00522980, -0.40f }, // W18 damage numbers / item text: pop out
        { 0x00522ADC, -0.15f }, // W19 cursor / smart-cursor overlay
        { 0x00522AF4, 0.00f },  // W20 build grid (on the tile plane)
    };
    constexpr u32 kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

    // Depth presets (v0.4.2). Same site order as kSites; preset 0 is kSites.
    // sky / overlay / hud are the stamp-0 depths of SegmentStartDepth().
    struct DepthPreset
    {
        const char *name;
        float sky, overlay, hud;
        float site[kSiteCount];
    };
    //                 B1    B2    B3    B4    B5    B6    W0    W1    W2    W3    W4    W5    W6    W7
    //                 W8    W8b   W8b'  W9    W10   W11   W12   W13   W14   W15   W16   W17   W18   W19   W20
    const DepthPreset kPresets[] = {
        // Default: everything behind the screen, like looking into a terrarium.
        // Easiest on the eyes, nothing pops out. Sky (B0, a vertical gradient)
        // stays at 0: its depth is invisible, and at 0.20 its shift exposed a
        // strip of the right eye's black clear at the left edge (v0.5.1).
        { "Terrarium Window (default)", 0.00f, 0.05f, 0.00f, {
            1.00f, 0.90f, 0.86f, 0.80f, 0.74f, 0.68f, 0.60f, 0.45f, 0.45f, 0.45f, 0.40f, 0.38f, 0.32f, 0.32f,
            0.20f, 0.20f, 0.20f, 0.20f, 0.12f, 0.18f, 0.18f, 0.22f, 0.10f, 0.06f, 0.20f, 0.20f, 0.04f, 0.10f, 0.20f } },
        // Play plane on the screen, effects pop out (uses the kSites depths).
        { "Diorama", 0.00f, -0.25f, -0.15f, {},
        },
        // The world floats in front of the screen, backgrounds just behind it.
        { "Pop-out (world in front)", 0.00f, -0.50f, -0.45f, {
            0.50f, 0.42f, 0.40f, 0.34f, 0.28f, 0.22f, 0.10f, -0.05f, -0.05f, -0.05f, -0.08f, -0.10f, -0.15f, -0.15f,
            -0.30f, -0.30f, -0.30f, -0.30f, -0.45f, -0.34f, -0.34f, -0.25f, -0.55f, -0.70f, -0.30f, -0.30f, -0.70f, -0.40f, -0.30f } },
        // Walls and trees almost on the ground plane: no see-through gaps
        // between walls and blocks at full slider, depth stays in backgrounds.
        { "Tight walls (no gaps)", 0.00f, -0.25f, -0.15f, {
            1.00f, 0.85f, 0.80f, 0.72f, 0.64f, 0.55f, 0.40f, 0.08f, 0.08f, 0.08f, 0.07f, 0.06f, 0.05f, 0.05f,
            0.00f, 0.00f, 0.00f, 0.00f, -0.12f, -0.04f, -0.04f, 0.03f, -0.25f, -0.40f, 0.00f, 0.00f, -0.40f, -0.15f, 0.00f } },
        // Very deep backgrounds and big effects, play plane on the screen.
        { "Cinematic (deep + big pops)", 0.00f, -0.35f, -0.20f, {
            1.00f, 1.00f, 0.95f, 0.90f, 0.85f, 0.80f, 0.60f, 0.35f, 0.35f, 0.35f, 0.30f, 0.27f, 0.22f, 0.22f,
            0.00f, 0.00f, 0.00f, 0.00f, -0.20f, -0.06f, -0.06f, 0.05f, -0.40f, -0.60f, 0.00f, 0.00f, -0.60f, -0.20f, 0.00f } },
        // Underground: the cavern backdrop recedes far behind the background
        // walls, torches and furniture sit just behind you, sparks pop out.
        { "Caves (deep tunnels)", 0.00f, -0.25f, -0.15f, {
            1.00f, 0.95f, 0.90f, 0.85f, 0.82f, 0.80f, 0.80f, 0.42f, 0.42f, 0.42f, 0.34f, 0.30f, 0.14f, 0.14f,
            0.00f, 0.00f, 0.00f, 0.00f, -0.15f, -0.04f, -0.04f, 0.05f, -0.30f, -0.50f, 0.00f, 0.00f, -0.40f, -0.12f, 0.00f } },
        // Building: shallow and precise. Cursor and build grid on the block
        // plane so the reticle lines up with tiles, effects barely pop.
        { "Builder (shallow, precise)", 0.00f, -0.05f, 0.00f, {
            0.60f, 0.55f, 0.50f, 0.45f, 0.40f, 0.35f, 0.25f, 0.10f, 0.10f, 0.10f, 0.08f, 0.07f, 0.06f, 0.06f,
            0.00f, 0.00f, 0.00f, 0.00f, -0.05f, -0.02f, -0.02f, 0.03f, -0.05f, -0.08f, 0.00f, 0.00f, -0.10f, 0.00f, 0.00f } },
        // Boss fights: enemies, you and projectiles step forward off the
        // terrain; gore, dust and damage numbers fly at your face.
        { "Combat (enemies forward)", 0.00f, -0.40f, -0.20f, {
            1.00f, 0.90f, 0.85f, 0.78f, 0.70f, 0.62f, 0.50f, 0.32f, 0.32f, 0.32f, 0.26f, 0.24f, 0.18f, 0.18f,
            0.00f, 0.00f, 0.00f, -0.12f, -0.35f, -0.14f, -0.08f, 0.05f, -0.45f, -0.55f, 0.00f, 0.00f, -0.65f, -0.20f, 0.00f } },
    };
    constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
    constexpr int kDioramaPreset = 1;
    static_assert(kSiteCount == 29, "update kPresets when kSites changes");

    float s_depth[kSiteCount];
    float s_sky = 0.00f, s_overlay = 0.05f, s_hud = 0.0f;
    int   s_preset = 0;

    // Title / menu background (MenuBackground::Draw 0x21B770, vtable 0x5D7168,
    // top screen only). It runs the same sky/background pass 0x17F418, so
    // B1..B6 above already match, but everything after it is a second batch
    // segment no table entry covered, so it all sat on the screen plane:
    //   segment 1: Begin(1) 0x21B800, 0x17F418, End -> LR 0x21B8B4
    //   segment 2: Begin(0) 0x21B8BC, drifting clouds 0x21A7A8 (random sprite
    //              ids 7000..7020), NextLayer 0x21B8D4 (LR 0x21B8D8), then
    //              0x21C0BC: island picture main_menu_bg (+0x8C) and the
    //              bobbing Logo_02 (+0x88); End -> LR 0x21B8F4
    // Depths follow the preset and never increase along the draw order
    // (clouds overlap the hills): clouds = nearest hills (B6), island = half of
    // that, logo = screen plane. Menu widgets (HUD containers outside the
    // world) are forced to the screen plane in t3d_end.
    constexpr u32 kStampMenuIsland = kSiteCount + 1;
    constexpr u32 kStampMenuLogo   = kSiteCount + 2;
    constexpr u32 kStampMax        = kStampMenuLogo;
    constexpr u32 kMenuLogoSlot    = 0x0021C160;   // ldr r0,[r4,#0x88]; cmp r0,#0
    float s_menuClouds = 0.68f, s_menuIsland = 0.34f;

    // Title walker (v0.5.1). The little player walking over the menu art is a
    // UI player-preview widget (vtable 0x5D77E4, ctor 0x23EB8C) owned by the
    // MenuBackground at +0xB4; its +0x18 Player is placed every frame through
    // the menu camera (0x21AB04..0x21AB38, same transform 0x21B680 as the
    // island). It draws in MenuBackground's widget pass (0x510A0C, after the
    // background's End) with its own Begin (0x23E5F8) / DrawPlayer 0x42BD3C /
    // End wrapper (LR 0x23E998), all on stamp 0, so it sat on the screen
    // plane with the logo. The same widget class is the character-select
    // preview, so only the instance at MenuBackground+0xB4 is moved: End
    // passes the caller's r4 (the widget, 0x23E52C) and the background's End
    // (LR 0x21B8F4, r4 = MenuBackground) records the owner each frame.
    constexpr u32 kEndLrMenuClouds  = 0x0021B8F4;
    constexpr u32 kEndLrPreview     = 0x0023E998;
    constexpr u32 kMenuBgWalker     = 0xB4;
    u32 s_menuBg = 0;               // MenuBackground drawn this frame (0 = none)

    // Edge fill (v0.5.1). A layer shifted right in the right eye exposes a
    // strip [0, dx) at the left edge that the game never drew (it culls to
    // the screen): bare sky beside the mountains in-world, and on the title
    // the 400x240 main_menu_bg art (camera-clamped to the screen edge) left a
    // sliver. While a background layer is being submitted (B2..B6, W0, menu
    // art) every sprite that touches the edge the shift uncovers gets one
    // extra mirrored copy just outside the screen: off-screen in the left eye
    // (left eye unchanged), and in the right eye it fills the strip with the
    // layer's own edge texels, continuous at the seam. Depth is unchanged.
    constexpr u32 kFnSpriteWrite = 0x001D90C4;
    constexpr u32 kFnRecordAlloc = 0x001D8D1C;   // SpriteBatch: next free record
    constexpr u32 kSbVerts       = 0x0C;         // mapped vertex buffer (4 x 0x14 B per sprite)
    constexpr u32 kSbCount       = 0x1C;         // records this segment
    constexpr u32 kSbVertBase    = 0x20;         // vertex index base
    constexpr u32 kSbZoomMode    = 0x48;         // u8: sizes scaled by +0x4C
    constexpr u32 kSbZoom        = 0x4C;
    constexpr float kScreenW     = 400.0f;
    constexpr float kMinFillW    = 8.0f;
    using FnRecAlloc = u32 (*)(u32 batch);
    const auto recordAlloc = reinterpret_cast<FnRecAlloc>(kFnRecordAlloc);
    bool s_edgeFill = true;
    u32  s_fills = 0, s_fillsLastFrame = 0;

    void ApplyPreset(int i)
    {
        s_preset = i;
        for (u32 s = 0; s < kSiteCount; s++)
            s_depth[s] = i == kDioramaPreset ? kSites[s].depth : kPresets[i].site[s];
        s_sky = kPresets[i].sky;
        s_overlay = kPresets[i].overlay;
        s_hud = kPresets[i].hud;
        s_menuClouds = s_depth[5];              // B6 nearest hills
        s_menuIsland = s_depth[5] * 0.5f;
    }

    // BeginFrame: the previous frame drew the world (W0 ran), i.e. not a menu.
    bool s_inWorld = false;
    u32  s_worldDrawsAtBegin = 0;

    // Depth of stamp 0 depends on which End flushed the segment (End's LR;
    // the wrapper 0x22019C tail-calls End so these are the wrapper callers).
    // B0 is a vertical gradient: a sideways shift is invisible, and keeping it
    // at 0 means it still covers the whole right eye, so the far layers' shift
    // no longer exposes a black band at the left edge (playtest).
    // Preset 0: sky 0.00 (world segment start = B0 sky gradient), overlay -0.25
    // (name tags, chat bubbles, boss bars), HUD -0.15 (-0.25 popped out too far).
    float SegmentStartDepth(u32 endLr)
    {
        switch (endLr)
        {
            case 0x0029D340: case 0x00522A80: return s_sky;
            case 0x003DB51C:                  return s_overlay;
            case 0x0050F300:                  return s_hud;   // debug OSD
            case 0x0047E9FC:                  return s_hud;   // HUD widget container
            // Menu sky segment (0x21B8B4) stays at 0: the gradient must cover
            // the whole right eye (no black edge band) and is shift-invariant.
            case 0x0021B8F4:                  return s_menuClouds; // menu clouds
        }
        return 0.0f;
    }

    // -----------------------------------------------------------------------
    bool    s_installed = false;
    bool    s_frameActive = false;
    float   s_sepPx = 0.0f;
    u32     s_cmdGuard = 0x3F000;
    u32     s_runs = 0, s_runsLastFrame = 0;

    u32     s_rightRes = 0;
    u32     s_rightRT = 0;
    bool    s_rtFailed = false;
    bool    s_rtInFcram = false;

    bool CreateRightTarget()
    {
        // Same descriptor as the game's top colour buffer (0x5C2C14):
        // 256x512, 1 level, RGBA8, 8-byte align, location (1 FCRAM, 2 VRAM-A, 4 VRAM-B)
        u32 desc[6] = { 0x100, 0x200, 1, 0, 8, 4 };
        u32 res = 0;
        if (gdCreateTexResource(desc, 0, 0, &res, 1) & 0x80000000)
        {
            desc[5] = 1;
            s_rtInFcram = true;
            res = 0;
            if (gdCreateTexResource(desc, 0, 0, &res, 1) & 0x80000000)
                return false;
        }
        const u32 mip0 = 0;
        u32 rt = 0;
        if (gdCreateRenderTarget(res, &mip0, &rt) & 0x80000000)
            return false;
        s_rightRes = res;
        s_rightRT = rt;
        return true;
    }

    // The sprite shader positions vertices with uProjection only (uModelView
    // is declared but a translation in it had no effect in testing), so the
    // per-layer shift goes into a copy of the game's projection:
    //   P' = P * T(dx)  ->  every row r: P'[r][3] = P[r][3] + P[r][0] * dx
    // (row-major, as built by the game's ortho builder 0x11D708). T is applied
    // before projection, so dx is in the game's screen pixels (horizontal).
    float   s_proj[16];
    bool    s_haveProj = false;
    bool    s_inReplay = false;
    t3d::hook::FunctionHook s_uniformHook;

    u32 Hooked_SetFloatUniform(u32 program, u32 location, const float *m)
    {
        if (!s_inReplay && location == R32(kGdLocProjection) && m)
        {
            for (int i = 0; i < 16; i++) s_proj[i] = m[i];
            s_haveProj = true;
        }
        return t3d::hook::FunctionHook::CallOriginal<u32>(program, location, m);
    }

    void UploadProjectionShiftX(float dx)
    {
        float m[16];
        for (int i = 0; i < 16; i++) m[i] = s_proj[i];
        for (int r = 0; r < 4; r++)
            m[r * 4 + 3] += m[r * 4 + 0] * dx;
        gdSetFloatUniform(R32(kGdProgram), R32(kGdLocProjection), m);
    }

    inline u32 TextureKey(u32 rec)
    {
        // rec[0] = sprite source; source->+0x10->+0x18 identifies the texture
        return R32(R32(R32(rec) + 0x10) + 0x18);
    }

    float   s_popScale = 1.0f;     // "Pop-out effects": scales negative depths

    // Hardware white-tile bug (v0.5.x, New 3DS, caves): A/B switches.
    //  s_safeMode 0 = normal per-layer replay
    //             1 = replay every run with NO shift (right eye = exact copy of
    //                 the left draw through the replay path: tests the replay
    //                 mechanics alone; the picture is flat)
    //             2 = no replay at all (stage-1 flat shift of the left image)
    //  s_snapPx   round each run's shift to whole screen pixels, so both eyes
    //             sample textures at the same sub-pixel phase (a fractional
    //             shift moves every pixel centre to a different texel position:
    //             edge texels / atlas neighbours can show in one eye only).
    int     s_safeMode = 0;
    bool    s_snapPx = true;

    inline float ShiftPx(float depth)
    {
        if (s_safeMode == 1)
            return 0.0f;
        const float dx = s_sepPx * depth;
        if (!s_snapPx)
            return dx;
        return static_cast<float>(static_cast<s32>(dx >= 0.0f ? dx + 0.5f : dx - 0.5f));
    }

    inline float Pop(float d) { return d < 0.0f ? d * s_popScale : d; }

    inline float RecordDepth(u32 rec, float stamp0Depth)
    {
        const u16 stamp = *reinterpret_cast<u16 *>(rec + kRecLayer);
        if (stamp == 0 || stamp > kStampMax)
            return Pop(stamp0Depth);
        if (stamp == kStampMenuIsland) return Pop(s_menuIsland);
        if (stamp == kStampMenuLogo)   return 0.0f;
        return Pop(s_depth[stamp - 1]);
    }

    // Replays [0, pending) of the segment exactly as the original End chunks
    // and indexes it, split additionally at depth changes, into the right RT.
    //
    // State parity: the original End binds, per texture run, the source of the
    // run's LAST record (recs[texEnd]). Depth splits cut a texture run into
    // pieces; each piece used to bind its own last record instead, i.e. a
    // sprite source the left eye never bound. Records in one texture run share
    // the texture handle (the run key), but bind takes the whole source
    // (0x119DBC -> (ScreenMgr+0x2C)->vt[0x28](src)), so any per-source state
    // would differ from the left eye. Every piece now binds recs[texEnd].
    void ReplayRightEye(u32 b, s32 pending, s32 cap, s32 flushed, float stamp0Depth, bool hud)
    {
        const u32 *recs = reinterpret_cast<const u32 *>(R32(b + kSbRecords));
        const s32 vbSlots = static_cast<s32>(R32(R32(b + kSbVertexBuf) + 8) >> 2);
        const u32 screenMgr = R32(b + kSbScreenMgr);
        float lastDx = 0.0f;
        bool  mvDirty = false;

        s_inReplay = true;
        gdSetRenderTarget(s_rightRT);

        for (s32 base = 0; base < pending; base += cap)
        {
            s32 n = pending - base < cap ? pending - base : cap;
            if (vbSlots - flushed < n)
                n = vbSlots - flushed;
            if (flushed == cap)  // mirrors the original's `+0x74*4 != +8*4` skip
                continue;

            screenLock(screenMgr);
            const s32 end = base + n;
            s32 runStart = base;
            s32 texEnd = base - 1;      // last record of the current texture run
            for (s32 i = base; i < end; i++)
            {
                const u32 rec = recs[i];
                const u32 next = (i + 1 < end) ? recs[i + 1] : 0;
                if (i > texEnd)
                {
                    // Same split rule as the original End (0x10FC1C..0x10FC5C)
                    texEnd = i;
                    while (texEnd < end - 1)
                    {
                        const u32 a = recs[texEnd], b2 = recs[texEnd + 1];
                        if (!a || !b2 || TextureKey(a) != TextureKey(b2))
                            break;
                        texEnd++;
                    }
                }
                const float depth = hud ? Pop(stamp0Depth) : (rec ? RecordDepth(rec, stamp0Depth) : 0.0f);
                bool split = (i >= end - 1) || !next || !rec || TextureKey(rec) != TextureKey(next);
                if (!split && !hud && RecordDepth(next, stamp0Depth) != depth)
                    split = true;
                if (!split)
                    continue;

                const float dx = ShiftPx(depth);
                if (dx != lastDx || !mvDirty)
                {
                    UploadProjectionShiftX(dx);
                    lastDx = dx;
                    mvDirty = true;
                }

                // Same guard sampling as the original End
                if (++S32(b + kSbSegCounter) == 10)
                {
                    nngxGetCmdlistParam(0x202, &R32(b + kSbCmdBytes));
                    S32(b + kSbSegCounter) = 0;
                }
                if (R32(b + kSbCmdBytes) < s_cmdGuard && rec)
                {
                    bindSpriteTexture(screenMgr, R32(recs[texEnd]), 0);
                    drawIndexed(static_cast<u32>(i - runStart + 1) * 6,
                                static_cast<u32>(flushed + (runStart - base)) * 6);
                    s_runs++;
                }
                runStart = i + 1;
            }
            screenUnlock(screenMgr);
            flushed += n;
        }

        if (mvDirty)
            gdSetFloatUniform(R32(kGdProgram), R32(kGdLocProjection), s_proj);
        gdSetRenderTarget(R32(kGdTopRT));
        s_inReplay = false;
    }
}

// ---------------------------------------------------------------------------
// Hooks (called from stereo_stubs.s)
// ---------------------------------------------------------------------------
extern "C" void t3d_end_original(u32 batch);
extern "C" void t3d_stub_layer_inc();
extern "C" void t3d_stub_end();
extern "C" void t3d_stub_menu_logo();
extern "C" void t3d_stub_sprite_write();
// A background layer is being submitted: the vertex-writer stub only enters
// t3d_sprite_write while this is set (4 instructions per sprite otherwise).
extern "C" { bool t3d_fill_armed = false; }
extern "C" void t3d_sprite_write_original(u32 batch, u32 rec, float *pos, const float *src, float rot,
                                          const u8 *color, const float *origin, const float *scale);

static volatile u32 s_worldDraws = 0;

extern "C" void t3d_layer_inc(u32 batch, u32 callerLr)
{
    if (callerLr == 0x0052267C)   // W0: the first layer of every world draw
        s_worldDraws = s_worldDraws + 1;
    // Give each table site a fixed stamp; every other caller (player parts,
    // NPC sub-sprites, tiles' internal passes, menus, ...) inherits its parent
    // layer. The counter is write-only in game code, so this is invisible.
    for (u32 i = 0; i < kSiteCount; i++)
    {
        if (kSites[i].lr == callerLr)
        {
            *reinterpret_cast<u16 *>(batch + kSbLayer) = static_cast<u16>(i + 1);
            // B2..B6 and W0: the screen-covering background layers
            t3d_fill_armed = s_frameActive && s_edgeFill && i >= 1 && i <= 6;
            return;
        }
    }
    if (callerLr == 0x0021B8D8)   // menu background: clouds done, island next
    {
        *reinterpret_cast<u16 *>(batch + kSbLayer) = static_cast<u16>(kStampMenuIsland);
        t3d_fill_armed = s_frameActive && s_edgeFill;
    }
}

// From t3d_stub_menu_logo at 0x21C160: island drawn, the logo comes next.
extern "C" void t3d_menu_logo_mark()
{
    const u32 batch = R32(0x0086D278);    // g_batch holder -> SpriteBatch*
    if (batch)
        *reinterpret_cast<u16 *>(batch + kSbLayer) = static_cast<u16>(kStampMenuLogo);
    t3d_fill_armed = false;
}

// Depth of the batch's current stamp, for sprites being submitted (stamp 0's
// depth depends on the End that will flush it: not known here, never armed).
static float SubmitDepth(u16 stamp)
{
    if (stamp == 0 || stamp > kStampMax) return 0.0f;
    if (stamp == kStampMenuIsland)       return Pop(s_menuIsland);
    if (stamp == kStampMenuLogo)         return 0.0f;
    return Pop(s_depth[stamp - 1]);
}

// Replaces the vertex writer 0x1D90C4 (veneer at its entry). Arguments as the
// game passes them: pos is transformed in place by the original; src = source
// rect {x,y,w,h}; color/origin/scale pointers are only read.
extern "C" void t3d_sprite_write(u32 batch, u32 rec, float *pos, const float *src, float rot,
                                 const u8 *color, const float *origin, const float *scale)
{
    if (!t3d_fill_armed)
    {
        t3d_sprite_write_original(batch, rec, pos, src, rot, color, origin, scale);
        return;
    }

    const float px = pos[0], py = pos[1];
    const u32 idx = R32(batch + kSbVertBase) + R32(batch + kSbCount);
    t3d_sprite_write_original(batch, rec, pos, src, rot, color, origin, scale);

    if (rot != 0.0f || idx >= R32(batch + kSbCapacity) || R32(kGdCurrentRT) != R32(kGdTopRT))
        return;
    const float dx = ShiftPx(SubmitDepth(*reinterpret_cast<const u16 *>(batch + kSbLayer)));
    if (dx == 0.0f)
        return;

    // Cheap reject before reading the vertex buffer back: the transformed
    // anchor (pos) lies within one sprite width of every corner.
    float wz = src[2] * scale[0];
    if (wz < 0.0f) wz = -wz;
    if (*reinterpret_cast<const u8 *>(batch + kSbZoomMode))
        wz *= *reinterpret_cast<const float *>(batch + kSbZoom);
    wz = wz * 2.0f + 1.0f;
    if (dx > 0.0f ? pos[0] > wz : pos[0] < kScreenW - wz)
        return;

    // Vertex: x, y (f32), rgba (u8 x4), u, v (f32) = 5 words
    const float *v = reinterpret_cast<const float *>(R32(batch + kSbVerts) + idx * 0x50);
    float xl = v[0], xr = v[0], ul = v[3], ur = v[3];
    for (int k = 1; k < 4; k++)
    {
        const float x = v[k * 5], u = v[k * 5 + 3];
        if (x < xl) xl = x;
        if (x > xr) xr = x;
        if (u < ul) ul = u;
        if (u > ur) ur = u;
    }
    const float w = xr - xl;
    if (w < kMinFillW)
        return;
    float move;
    if (dx > 0.0f && xl <= 0.0f && xl + dx > 0.0f && xr > 0.0f)
        move = -w;          // copy just left of the screen
    else if (dx < 0.0f && xr >= kScreenW && xr + dx < kScreenW && xl < kScreenW)
        move = w;           // in-front layer: copy just right of the screen
    else
        return;

    // Second sprite: same record contents (texture source), same arguments,
    // so it lands on the same stamp and texture run, right after the first.
    const u32 idx2 = R32(batch + kSbVertBase) + R32(batch + kSbCount);
    if (idx2 >= R32(batch + kSbCapacity))
        return;
    const u32 rec2 = recordAlloc(batch);
    if (!rec2)
        return;
    for (int i = 0; i < 9; i++)                 // +0x00..+0x20, as the entry sets them
        R32(rec2 + i * 4) = R32(rec + i * 4);
    float pos2[2] = { px, py };
    t3d_sprite_write_original(batch, rec2, pos2, src, rot, color, origin, scale);

    // Move it beside the original and mirror its u, so the copy's column at
    // the seam is the original's edge column.
    float *v2 = reinterpret_cast<float *>(R32(batch + kSbVerts) + idx2 * 0x50);
    for (int k = 0; k < 4; k++)
    {
        v2[k * 5] += move;
        v2[k * 5 + 3] = ul + ur - v2[k * 5 + 3];
    }
    s_fills++;
}

extern "C" void t3d_end(u32 batch, u32 callerLr, u32 callerR4)
{
    t3d_fill_armed = false;
    if (callerLr == kEndLrMenuClouds)
        s_menuBg = callerR4;        // MenuBackground::Draw's `this`
    const s32 pending = S32(batch + kSbPending);
    if (!s_frameActive || !s_haveProj || pending <= 0 || R32(kGdCurrentRT) != R32(kGdTopRT))
    {
        t3d_end_original(batch);
        return;
    }

    const s32 cap = S32(batch + kSbCapacity);
    const s32 flushed = S32(batch + kSbFlushed);
    // HUD segments are recognised by their End call site, not the batch: the
    // HUD and the world share one SpriteBatch (*0x633B3C == *0x86D278).
    const bool hud = callerLr == 0x0047E9FC;
    float stamp0 = SegmentStartDepth(callerLr);
    if (hud && !s_inWorld)
        stamp0 = 0.0f;          // menu widgets (title, select screens): screen plane
    if (callerLr == kEndLrPreview && s_menuBg && R32(s_menuBg + kMenuBgWalker) == callerR4)
        stamp0 = s_menuIsland;  // title walker: at the depth of the art it walks on

    t3d_end_original(batch);    // left eye: untouched original flush
    const u64 t0 = svcGetSystemTick();
    ReplayRightEye(batch, pending, cap, flushed, stamp0, hud);
    t3d::perf::AddReplayTicks(svcGetSystemTick() - t0);
}

// ---------------------------------------------------------------------------
namespace t3d::stereo::layers
{
    bool Install()
    {
        if (s_installed)
            return true;
        ApplyPreset(s_preset);

        // All words must be exactly the v5.6.0 originals before touching anything.
        u32 w[4];
        for (int i = 0; i < 4; i++) mem::Read32(kFnLayerInc + i * 4, w[i]);
        if (w[0] != 0xE1D016BC || w[1] != 0xE2811001 || w[2] != 0xE1C016BC || w[3] != 0xE12FFF1E)
            return false;
        for (int i = 0; i < 4; i++) mem::Read32(kFnLayerDec + i * 4, w[i]);
        if (w[0] != 0xE1D016BC || w[1] != 0xE2411001 || w[2] != 0xE1C016BC || w[3] != 0xE12FFF1E)
            return false;
        mem::Read32(kFnEnd, w[0]);
        mem::Read32(kFnEnd + 4, w[1]);
        if (w[0] != 0xE92D4FF0 || w[1] != 0xE1A05000)
            return false;

        // The command list stays at the game's own 0x40000 bytes. v0.3.0 doubled
        // it (512 KiB of the game's FCRAM device heap) and the world map then
        // failed to allocate its FCRAM texture and the game panicked (hardware
        // dump: svcBreak after CreateTexture2DResource at 0x26ABF0). Measured
        // peak with per-layer stereo: ~0x1D800 of 0x40000; the per-second log
        // records the peak (perf::NoteCmdBytes) so a busy scene that nears the
        // 0x3F000 guard (sprites dropped, no crash) shows up.
        (void)kCmdlistSizeInsn;
        (void)kCmdlistGuardInsn;

        // NextLayer -> stamp stub; PreviousLayer -> no-op (counter is cosmetic)
        if (!mem::PatchAbsJump(kFnLayerInc, reinterpret_cast<u32>(t3d_stub_layer_inc)))
            return false;
        mem::Write32(kFnLayerDec, mem::kInsnBxLr);
        if (!mem::PatchAbsJump(kFnEnd, reinterpret_cast<u32>(t3d_stub_end)))
            return false;

        // Edge fill: veneer over the vertex writer's entry (push {r4-r10,lr};
        // mov r6,r0). Optional: without it, layers show the bare edge again.
        mem::Read32(kFnSpriteWrite, w[0]);
        mem::Read32(kFnSpriteWrite + 4, w[1]);
        mem::Read32(kFnRecordAlloc, w[2]);
        if (w[0] == 0xE92D47F0 && w[1] == 0xE1A06000 && w[2] == 0xE92D47F0)
            mem::PatchAbsJump(kFnSpriteWrite, reinterpret_cast<u32>(t3d_stub_sprite_write));

        // Title logo on its own stamp. Optional: without it the logo shares the
        // island's depth. 0x21C160 is a branch target (0x21C0DC beq), which is
        // fine: the veneer starts there; 0x21C164 is not a target.
        mem::Read32(kMenuLogoSlot, w[0]);
        mem::Read32(kMenuLogoSlot + 4, w[1]);
        if (w[0] == 0xE5940088 && w[1] == 0xE3500000)
            mem::PatchAbsJump(kMenuLogoSlot, reinterpret_cast<u32>(t3d_stub_menu_logo));

        // Capture the game's projection (uploaded by BeginTop every frame)
        if (s_uniformHook.Install(0x0011B24C, reinterpret_cast<void *>(Hooked_SetFloatUniform),
                                  0xE92D4FF8) != t3d::hook::Result::Ok)
            return false;

        s_installed = true;
        return true;
    }

    bool Installed() { return s_installed; }

    u32  WorldDrawCount()          { return s_worldDraws; }

    void  SetPopScale(float scale) { s_popScale = scale; }

    int         PresetCount()       { return kPresetCount; }
    int         Preset()            { return s_preset; }
    const char *PresetName(int i)   { return i >= 0 && i < kPresetCount ? kPresets[i].name : "?"; }
    void        SetPreset(int i)    { if (i >= 0 && i < kPresetCount) ApplyPreset(i); }
    float PopScale()               { return s_popScale; }

    void ReserveRightTarget()
    {
        // Called on the first displayed frame (title screen), while VRAM-B is
        // still free; see stereo_present.cpp OnPreTop.
        if (s_installed && !s_rightRT && !s_rtFailed && !CreateRightTarget())
            s_rtFailed = true;
    }

    void BeginFrame(bool on, float sepPx)
    {
        s_runsLastFrame = s_runs;
        s_runs = 0;
        s_fillsLastFrame = s_fills;
        s_fills = 0;
        t3d_fill_armed = false;
        s_menuBg = 0;
        s_inWorld = s_worldDraws != s_worldDrawsAtBegin;
        s_worldDrawsAtBegin = s_worldDraws;
        s_frameActive = false;
        s_sepPx = sepPx;
        if (!s_installed || !on || s_rtFailed || s_safeMode == 2)
            return;     // safe mode 2: present falls back to the flat stage-1 copy

        if (!s_rightRT && !CreateRightTarget())
        {
            s_rtFailed = true;
            return;
        }

        static const u32 kClearColor = 0xFF000000; // game's top clear colour (0x108DD8)
        gdClearTargets(s_rightRT, 0, &kClearColor, 0.0f);
        s_frameActive = true;
    }

    u32  RightColorResourceThisFrame() { return s_frameActive ? s_rightRes : 0; }
    bool RightTargetFailed()           { return s_rtFailed; }
    bool RightTargetInFcram()          { return s_rtInFcram; }
    u32  ReplayedRunsLastFrame()       { return s_runsLastFrame; }
    u32  EdgeFillsLastFrame()          { return s_fillsLastFrame; }
    void SetEdgeFill(bool on)          { s_edgeFill = on; }
    bool EdgeFill()                    { return s_edgeFill; }

    namespace
    {
        const char *const kSafeModeNames[] = {
            "Off (normal per-layer depth)",
            "Replay without shift (flat, tests the replay)",
            "Flat shift, no replay (most stable)",
        };
    }
    int         SafeModeCount()        { return 3; }
    const char *SafeModeName(int m)    { return m >= 0 && m < 3 ? kSafeModeNames[m] : "?"; }
    int         SafeMode()             { return s_safeMode; }
    void        SetSafeMode(int m)     { if (m >= 0 && m < 3) s_safeMode = m; }
    void        SetSnapPixels(bool on) { s_snapPx = on; }
    bool        SnapPixels()           { return s_snapPx; }
}
