#include "features.hpp"

#include <CTRPluginFramework.hpp>

#include "hooks.hpp"
#include "memory.hpp"
#include "perf.hpp"
#include "stereo_layers.hpp"

using namespace CTRPluginFramework;

#ifdef T3D_O3DS
// Old 3DS build: the C-stick / ZL / ZR / ir:rst paths are compiled but never
// reached (no hooks, no menu entries), so their helpers are unused here.
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

namespace t3d::features
{
    namespace
    {
        inline volatile u8    &U8(u32 a)  { return *reinterpret_cast<volatile u8 *>(a); }
        inline volatile s16   &S16(u32 a) { return *reinterpret_cast<volatile s16 *>(a); }
        inline volatile u32   &U32(u32 a) { return *reinterpret_cast<volatile u32 *>(a); }
        inline volatile float &F32(u32 a) { return *reinterpret_cast<volatile float *>(a); }

        // ---- game addresses (v5.6.0) ----
        constexpr u32 kCamera          = 0x008D4BDC; // Camera2D singleton (object, not pointer)
        constexpr u32 kCameraGuard     = 0x006398A4; // static-init guard byte (bit 0 = constructed)
        constexpr u32 kCamZoom         = kCamera + 0x14;
        constexpr u32 kCamZoomTarget   = kCamera + 0x18;
        constexpr float kZoomNormal    = 0.5f;       // gameplay default (half-size textures drawn 2x)
        constexpr float kZoom2x        = 1.0f;       // exact 2x, nearest-neighbour

        constexpr u32 kGsmGuard        = 0x006398D8; // GameStateManager guard
        constexpr u32 kCheatKeyboard   = 0x008D5324; // GSM+0x78: 1 = open the cheat keyboard next frame
        constexpr u32 kDevButtonBranch = 0x001F4674; // bne -> b: always show the "dev" settings group

        constexpr u32 kDebug           = 0x00633390; // D: debug flags struct
        constexpr u32 kDebugTools      = kDebug + 0x3; // "debug": cursor tile tools
        constexpr u32 kFullbright      = kDebug + 0x4; // "light"
        constexpr u32 kNoSpawns        = kDebug + 0x5; // "nospawns"
        constexpr u32 kGodmode         = kDebug + 0x6; // "godmode"
        constexpr u32 kInfBuckets      = kDebug + 0xA; // "infinitebuckets"

        constexpr u32 kPlayerArray     = 0x008B4080; // Player *[] indexed by myPlayer
        constexpr u32 kMyPlayer        = 0x00633408; // D+0x78
        constexpr u32 kPlayerVelX      = 0x294, kPlayerVelY = 0x298;
        constexpr u32 kPlayerPosY      = 0x288;
        constexpr u32 kPlayerFallStart = 0x65CC;     // s16 tile row
        constexpr u32 kFnPlayerMove    = 0x0042841C; // called after gravity, before collision

        constexpr u32 kTimeRate        = 0x0088D304; // float, time-of-day speed
        constexpr u32 kTime            = 0x0088D308; // float
        constexpr u32 kDayTime         = 0x0088D30C; // u8, 1 = day
        constexpr u32 kReachX          = 0x00264054; // float in .text (128.0 = 8 tiles)
        constexpr u32 kReachY          = 0x00264050; // float in .text (96.0 = 6 tiles)

        struct Patch { u32 addr, original, patched; };

        constexpr Patch kInfiniteItems[] = {
            { 0x00441F20, 0x0A000020, 0xEA000020 }, // consumables: blocks, walls, potions, thrown
            { 0x00441EB0, 0xDA000018, 0xEA000018 }, // wand ammo
            { 0x0043B354, 0xE2400001, 0xE2400000 }, // ammo
            { 0x0043D5B4, 0xE2411001, 0xE2411000 }, // red wire
            { 0x0043D630, 0xE2411001, 0xE2411000 }, // blue wire
            { 0x0043D6B4, 0xE2411001, 0xE2411000 }, // green wire
            { 0x0043D784, 0xE2411001, 0xE2411000 }, // actuators
            { 0x00408344, 0xE2411001, 0xE2411000 }, // wall auto-fill
            { 0x00282398, 0xE2411001, 0xE2411000 }, // recall potion
            { 0x00282470, 0xE2411001, 0xE2411000 }, // teleport potion
        };
        constexpr Patch kNoFallDamage[] = { { 0x004340AC, 0x1A00001F, 0xEA00001F } };
        constexpr Patch kInstantMine[]  = {
            { 0x00408904, 0xBA000028, 0xE320F000 }, // pickaxe
            { 0x0043E3C0, 0xBA00000E, 0xE320F000 }, // axe
        };

        template <size_t N>
        bool ApplyPatches(const Patch (&list)[N], bool on)
        {
            bool all = true;
            for (const Patch &p : list)
            {
                u32 cur = 0;
                mem::Read32(p.addr, cur);
                if (cur == (on ? p.patched : p.original))
                    continue;   // already in the wanted state
                all &= mem::Replace32(p.addr, on ? p.original : p.patched, on ? p.patched : p.original);
            }
            return all;
        }

        // ---- state (menu thread writes, game thread reads) ----
        volatile bool s_zoom2x = false, s_zoomDirty = false;
        volatile bool s_godmode = false, s_noSpawns = false, s_fullbright = false;
        volatile bool s_infBuckets = false, s_debugTools = false;
        volatile bool s_freezeTime = false, s_flight = false;
        bool  s_prevFlags[5] = {};
        bool  s_timeFrozenApplied = false;
        float s_savedTimeRate = 1.0f;

        hook::FunctionHook s_flightHook;

        u32 LocalPlayer()
        {
            const s32 idx = *reinterpret_cast<volatile s32 *>(kMyPlayer);
            if (idx < 0 || idx > 255)
                return 0;
            return U32(kPlayerArray + 4 * idx);
        }

        void Hooked_PlayerMove(u32 player)
        {
            if (s_flight && player && player == LocalPlayer())
            {
                constexpr float kSpeed = 6.0f;
                float vx = 0, vy = 0;
                if (Controller::IsKeyDown(Key::CPadLeft))  vx -= kSpeed;
                if (Controller::IsKeyDown(Key::CPadRight)) vx += kSpeed;
                if (Controller::IsKeyDown(Key::CPadUp))    vy -= kSpeed;
                if (Controller::IsKeyDown(Key::CPadDown))  vy += kSpeed;
                F32(player + kPlayerVelX) = vx;
                F32(player + kPlayerVelY) = vy;
                S16(player + kPlayerFallStart) = static_cast<s16>(static_cast<s32>(F32(player + kPlayerPosY)) >> 4);
            }
            hook::FunctionHook::CallOriginal<void>(player);
        }

        // ---- C-stick aiming (notes/aiming.md, variant B) ----
        // ReadLatest (0x2B4674) fills the extra-pad status at 0x8D85C8 once per
        // frame before any world logic: +4/+6 = C-stick X/Y (s16, about ±105,
        // up = +), +8 = held keys. Everything that aims (projectiles, melee
        // facing, grapple, mining target, placing) asks GetAimPoint (0x26507C),
        // which returns the cursor's C+8/+0xC (world px) when C+5 is set.
        constexpr u32 kFnPadReadLatest = 0x002B4674; // E92D4070, (reader*, status*) -> bool
        constexpr u32 kFnGetAimPoint   = 0x0026507C; // E92D4030, (float out[2], Cursor*)
        constexpr u32 kCursorPtr       = 0x008D84C0; // Cursor *
        constexpr u32 kPlayerPosX      = 0x284;      // float, top-left (world px)
        constexpr u32 kPlayerWidth     = 0x53D0;     // u16
        constexpr u32 kPlayerHeight    = 0x53D2;     // u16
        constexpr u32 kPlayerInventory = 0x9D0;      // Item[], 0x84 bytes each
        constexpr u32 kPlayerSelected  = 0x2C2;      // s8 selected hotbar slot
        constexpr u32 kFnItemCheck     = 0x00437EE8; // E92D4FF3, (Player*, int i); called at 0x4164B4
        constexpr u32 kKeyB            = 0x0002;
        constexpr u32 kKeyY            = 0x0800;
        constexpr u32 kKeyZL           = 0x4000;     // same bits in the game's pad and libctru
        constexpr u32 kKeyZR           = 0x8000;
        constexpr float kStickFull     = 105.0f;
        constexpr float kAimDeadzone   = 0.25f;
        constexpr float kAutoUseAt     = 0.85f;      // "pushed hard" (weapons, other items, Precise tools)
        // Auto-tunnel tools start digging at a smaller push (v0.5.4, Matt
        // 14:00-14:41: "60-something percent of the way there" was too far).
        constexpr float kAutoUseTunnelAt = 0.50f;
        constexpr float kAutoUseHyst   = 0.05f;      // keep swinging until the push drops this far below
        constexpr float kJitterDz      = 10.0f;      // raw stick units: below this the stick is centred

        // ---- control scheme (v0.5 notes in notes/aiming.md §6) ----
        // InputMapper button index (FUN_003fa790): A1 B2 X3 Y4 R5 ZR6 L8 ZL9 ...
        // Scheme object 0x8D5364 (guard 0x6398E0): +0x1C = scheme (5 Old 3DS,
        // 6 New 3DS); +5 = grapple button (read directly by the player controls
        // at 0x41C9B8, "pressed" edge); control id k's button at +0x28 + k*0xC.
        // New 3DS stock: control 0x12 = ZR (scheme +9), control 0x2D = ZL
        // (scheme +0xA). Their only users (v0.5.1 audit, notes/aiming.md §6.1):
        //   MiniInventoryBottom::Update 0x257224: ZR -> page 1 (inventory),
        //                                         ZL -> page 0 (build view)
        //   BuildModeBottom::Update     0x222040: ZL -> page 9
        //   inventory menu              0x27DD00: ZR closes, +8/+9 switch tabs
        constexpr u32 kSchemeGuard     = 0x006398E0;
        constexpr u32 kScheme          = 0x008D5364;
        constexpr u32 kSchemeGrapple   = kScheme + 5;
        constexpr u32 SchemeEntry(u32 id) { return kScheme + 0x28 + id * 0xC; }
        constexpr u32 kCtlInventory    = 0x12;       // stock ZR
        constexpr u32 kCtlMiniInv      = 0x2D;       // stock ZL
        constexpr u8  kBtnZR = 6, kBtnZL = 9;
        // The frame's button set: FUN_003FA790 turns the pad's held mask into
        // a 17-entry vector of {u8 down, float value} by calling 0x47040C once
        // per pad button. Index 0xA comes from no pad bit, no scheme entry and
        // no direct check, so the mod can use it as a private grapple button
        // and ZL/ZR can be hidden from the game completely.
        constexpr u32 kFnButtonSetPut  = 0x0047040C; // E5D11000, (vector*, u8 *index, entry *value)
        constexpr u32 kActionSetLive   = 0x008D4C18 + 0x1E0; // InputMapper's own set (FUN_001EDB74 writes it)
        constexpr u8  kBtnModGrapple   = 0xA;
        // Bottom-screen UI 0x8D4AE8 (guard 0x639888): +0x22 page, +0x23 target
        // page. The world opens on page 9 (MiniInventoryBottom, 0x3DCC04): that
        // is normal play. ZL toggles it with page 0 ("hudstatebottom" =
        // BuildModeBottom, the touch build view), also play. 1 inventory,
        // 6 pause, 7 chest... are menus and keep the stock ZL/ZR.
        constexpr u32 kUiGuard         = 0x00639888;
        constexpr u32 kUiPage          = 0x008D4AE8 + 0x22;
        constexpr u32 kUiPageTarget    = 0x008D4AE8 + 0x23;
        constexpr u8  kPageBuildView   = 0, kPageMiniInv = 9;

        enum CstickMode { kCstickOff = 0, kCstickAim = 1, kCstickAimAuto = 2 };
        enum TrigAction { kTrigDefault = 0, kTrigGrapple = 1, kTrigJump = 2, kTrigUse = 3, kTrigStyle = 4 };
        enum Preset     { kPresetNew3DS = 0, kPresetOld3DS = 1, kPresetStock = 2, kPresetLefty = 3,
                          kPresetNew3DSGrapple = 4, kPresetCount };

        hook::FunctionHook s_padHook, s_aimHook, s_itemCheckHook, s_buttonSetHook;
        volatile bool s_buttonSetHooked = false;        // private grapple button available
        volatile bool s_modGrappleDown = false;         // private grapple button this frame
        bool          s_trigOwned[2] = {};              // ZL, ZR press belongs to the mod until released
        bool          s_trigPrevRaw[2] = {};            // ZL, ZR raw state last tick (press edge)
        bool          s_autoUsing = false;              // hard-push auto-use active last tick (hysteresis)
        // D-pad walking (v0.5.4, off by default): in play the D-pad drives the
        // circle pad value and is hidden from the game (no D-pad cursor).
        volatile bool s_dpadWalk = false;
        bool          s_dpadWalking = false;
#ifdef T3D_O3DS
        // Old 3DS build: the "Old 3DS (D-pad + touch)" preset from the start,
        // fixed (no C-stick, ZL/ZR stock, no ir:rst).
        volatile int  s_cstickMode = kCstickOff;
        volatile bool s_padInteractOnly = false;
        volatile int  s_zlAction = kTrigDefault, s_zrAction = kTrigDefault;
        volatile int  s_sensitivity = 1;
        volatile int  s_smoothing = 2;
        volatile int  s_preset = kPresetOld3DS;
#else
        volatile int  s_cstickMode = kCstickAimAuto;
        volatile bool s_padInteractOnly = false;
        volatile int  s_zlAction = kTrigStyle, s_zrAction = kTrigUse;
        volatile int  s_sensitivity = 1;                // 0 low, 1 normal, 2 high
        volatile int  s_smoothing = 2;                  // 0 off, 1 light, 2 medium, 3 strong
        volatile int  s_preset = -1;                    // -1 = not chosen yet (auto-detect)
#endif
        volatile s16  s_stickX = 0, s_stickY = 0;       // raw merged stick (game or ir:rst)
        volatile float s_smX = 0, s_smY = 0;            // smoothed stick: what aiming uses
        volatile s16  s_gameX = 0, s_gameY = 0;         // what the game's extra pad reported
        volatile s16  s_irX = 0, s_irY = 0;             // ir:rst fallback (plugin side)
        volatile u32  s_irKeys = 0;                     // ir:rst ZL/ZR (plugin side)
        volatile bool s_irActive = false;
        volatile u32  s_padCalls = 0, s_aimCalls = 0, s_notConnectedFrames = 0;
        volatile u8   s_padConnected = 0;
        volatile bool s_useHeld = false;                // use (Y) held in play this frame
        u32           s_trigHeldFrames[2] = {};         // ZL, ZR held in play (game thread)
        constexpr u32 kRegrappleFrames = 12;            // re-press a held grapple trigger every 0.2 s

        // ---- tiles (for block placement along the aim) ----
        // Tile grid: *(u32*)0x7C4B20 = base, +0x10 = column stride (maxTilesY);
        // tile = base + (x*stride + y) * 0xE, +1 bit 0 = active (PlaceThing
        // 0x407030 / FUN_004f6278). maxTilesX/Y s16 at 0x6333A6 / 0x6333A8.
        constexpr u32 kTiles     = 0x007C4B20;
        constexpr u32 kMaxTilesX = 0x006333A6, kMaxTilesY = 0x006333A8;

        // -1 = outside the world / no grid, 0 = empty, 1 = active
        int TileState(int x, int y)
        {
            const u32 base = U32(kTiles);
            const s32 stride = *reinterpret_cast<volatile s32 *>(kTiles + 0x10);
            if (!base || x < 0 || y < 0 || x >= S16(kMaxTilesX) || y >= S16(kMaxTilesY) || y >= stride)
                return -1;
            return U8(base + (static_cast<u32>(x) * stride + y) * 0xE + 1) & 1;
        }
        bool          s_aimWasActive = false;

        // ---- C-stick style "Cursor (holds position)" ----
        // The stick moves the aim like a mouse (velocity ~ deflection, with an
        // acceleration curve) and the aim stays put when the stick is let go.
        // Stored as an offset from the player centre, so it walks with you,
        // clamped to the game's reach box. "Parked" (not placed) after a reset:
        // the smart cursor works as stock until the stick, D-pad or touch moves
        // it. Cursor::Draw (0x264C18) draws the "crosshair" at C+8/+C whenever
        // C+5 is set, and every tick the player controls ask GetAimPoint, so
        // keeping C+4/C+5 set keeps the crosshair on the held point.
        // Quick switch in play: ZL in the New 3DS preset (v0.5.4; the v0.5.2
        // Y+ZR combo is gone, it collided with mining). notes/aiming.md §6.4.
        enum CstickStyle { kStyleSpring = 0, kStyleCursor = 1 };
        constexpr float kHoldDeadzone  = 0.15f;
        // +50% after the 2026-10-28 hardware test ("up it a decent
        // amount ... don't go bananas"); was 90 / 130 / 200.
        constexpr float kHoldSpeed[]   = { 135.0f, 195.0f, 300.0f }; // px/s at full push
        constexpr float kTickDt        = 1.0f / 60.0f;               // ReadLatest runs once per game tick
        constexpr u32   kKeyDpad       = 0x00F0;
        volatile int  s_cstickStyle = kStyleSpring;
        volatile int  s_cursorSpeed = 1;                // 0 slow, 1 normal, 2 fast
        volatile bool s_holdResetReq = false;           // menu thread asks the game thread to reset
        volatile bool s_styleNotify = false;            // game thread asks the plugin thread for an OSD
        float         s_holdX = 0, s_holdY = 0;         // held aim, offset from the player centre
        bool          s_holdPlaced = false;
        bool          s_holdHidden = false;             // placed but hidden: the stock smart cursor is back

        // ---- after v0.5.2 (notes/aiming.md §6.3) ----
        // Cursor auto-hide: the held crosshair hides when you walk with the
        // circle pad and leave the C-stick alone (Matt 03:52-04:17), and comes
        // back at the same spot on the next C-stick push, D-pad step or touch.
        enum CursorHide { kHideWalking = 0, kHideWalkingOrIdle = 1, kHideNever = 2 };
        constexpr u32 kHideGraceTicks = 15;             // C-stick idle this long before walking hides it
        constexpr u32 kHideIdleTicks  = 180;            // "or idle": 3 s with no C-stick
        volatile int  s_cursorHide = kHideWalking;
        u32           s_cstickIdleTicks = 0;
        // Y button: stock (Matt 13:53 "I still want to break stuff with Y").
        // A Y press made while the C-stick is centred hands the aim back to the
        // game's smart cursor until Y is released.
        enum YButton { kYStock = 0, kYFollowsAim = 1 };
        volatile int  s_yButton = kYStock;
        bool          s_yStockLatch = false, s_prevPhysY = false;
        // C-stick with a tool (pick/axe/hammer), any style except Cursor:
        // steer the game's own smart-dig (FUN_0041F500, three parallel rays =
        // the 3-tall "auto tunnel") with the C-stick direction instead of
        // aiming at a point (Matt 14:06-14:36). Reach is the game's own.
        enum ToolMining { kToolTunnel = 0, kToolPrecise = 1 };
        volatile int  s_toolMining = kToolTunnel;
        volatile bool s_tunnelActive = false;
        volatile bool s_smartHooked = false;            // smart-cursor hook installed (tunnel needs it)
        volatile float s_tunnelDX = 0, s_tunnelDY = 0;  // unit direction, world (+y down)
        constexpr u32 kFnSmartCursor  = 0x00263B98;     // E92D41F0, (float out[2], Cursor*, float dir[2]); sole caller 0x264AB4
        // Plugin menu closed -> the game opens its pause page (seen in Azahar
        // 2026-10-29: page 9 -> 6 after Select). Resume once if the menu was
        // opened from play: one A press on the default "Return to Game".
        volatile bool s_menuFromPlay = false;
        volatile u32  s_resumeWindow = 0;       // ticks left to see the pause page after a close
        constexpr u32 kResumeTicks   = 45;
        constexpr u8  kPagePause     = 6;
        constexpr u32 kKeyA          = 0x0001;
        u32           s_resumePress  = 0;       // ticks of injected A left
        bool          s_holdWasInWorld = false;
        u32           s_holdPlayer = 0;
        u32           s_padTick = 0, s_holdSeenTick = ~0u;
        bool          s_dpadHeld = false;

        // The game's reach box around the player centre (FUN_00263f9c):
        // x +-reachX, y -(reachY+8)..+(reachY-8). Read the live literals so the
        // "Long reach" cheat widens it too.
        void ReachBox(float &side, float &up, float &down)
        {
            float rx = F32(kReachX), ry = F32(kReachY);
            if (!(rx >= 16.0f && rx <= 4096.0f)) rx = 128.0f;
            if (!(ry >= 16.0f && ry <= 4096.0f)) ry = 96.0f;
            side = rx;
            up = ry + 8.0f;
            down = ry - 8.0f;
        }

        void ClampHold()
        {
            float side, up, down;
            ReachBox(side, up, down);
            s_holdX = s_holdX < -side ? -side : s_holdX > side ? side : s_holdX;
            s_holdY = s_holdY < -up ? -up : s_holdY > down ? down : s_holdY;
        }

        void ResetHold()
        {
            s_holdX = 0.0f;
            s_holdY = 0.0f;
            s_holdPlaced = false;
            s_holdHidden = false;
        }

        bool CirclePadMoving()
        {
            return s_dpadWalking
                || Controller::IsKeyDown(Key::CPadLeft) || Controller::IsKeyDown(Key::CPadRight)
                || Controller::IsKeyDown(Key::CPadUp)   || Controller::IsKeyDown(Key::CPadDown);
        }
        u32           s_lastWorldDraws = 0;
        constexpr u32 kWorldDrawGraceTicks = 8;         // ticks without a world draw still "in world"
        u32           s_ticksSinceWorldDraw = kWorldDrawGraceTicks;

        constexpr float kSensGain[]    = { 0.70f, 0.85f, 1.00f };  // slope near the centre
        constexpr float kSmoothAlpha[] = { 1.00f, 0.35f, 0.27f, 0.20f };
        constexpr int   kSmoothLevels  = 4;

        // A byte of game data we override and later put back. If the game
        // rewrites it (scheme rebuilt), the new value becomes the original.
        struct ByteOverride
        {
            u32  addr;
            u8   orig = 0, written = 0;
            bool active = false;

            void Set(bool want, u8 value)
            {
                const u8 cur = U8(addr);
                if (want)
                {
                    if (!active || cur != written)
                        orig = cur;
                    if (cur != value)
                        U8(addr) = value;
                    written = value;
                    active = true;
                }
                else if (active)
                {
                    if (cur == written)
                        U8(addr) = orig;
                    active = false;
                }
            }
        };
        ByteOverride s_ovGrapple { kSchemeGrapple };
        ByteOverride s_ovInventory { SchemeEntry(kCtlInventory) };
        ByteOverride s_ovMiniInv { SchemeEntry(kCtlMiniInv) };

        // On hardware the game's C-stick arrives over ir:USER as an emulated
        // Circle Pad Pro (notes/aiming.md 1.4). If that link reports nothing,
        // read the stick from the system's ir:rst service instead (the game
        // never opens ir:rst). Raw ir:rst values run to about ±146.
        s16 ScaleIr(s16 v)
        {
            s32 s = static_cast<s32>(v) * 105 / 140;
            return static_cast<s16>(s > 105 ? 105 : s < -105 ? -105 : s);
        }

        // Once per game frame: pick the source, then a small radial deadzone
        // and exponential smoothing so the cursor doesn't jitter. Letting go
        // snaps straight to centre (no slow drift back through the player).
        void MergeStick()
        {
            const bool gameIdle = s_gameX == 0 && s_gameY == 0;
            s_stickX = gameIdle && s_irActive ? s_irX : s_gameX;
            s_stickY = gameIdle && s_irActive ? s_irY : s_gameY;
            const float rx = s_stickX, ry = s_stickY;
            if (rx * rx + ry * ry < kJitterDz * kJitterDz)
            {
                s_smX = 0.0f;
                s_smY = 0.0f;
                return;
            }
            const int   sm = s_smoothing;
            const float a  = kSmoothAlpha[sm < 0 || sm >= kSmoothLevels ? 2 : sm];
            s_smX = s_smX + (rx - s_smX) * a;
            s_smY = s_smY + (ry - s_smY) * a;
        }

        // A play page (normal play or the build view) that isn't mid-switch.
        bool UiOnPlayPage()
        {
            if ((U8(kUiGuard) & 1) == 0)
                return false;
            const u8 page = U8(kUiPage);
            return page == U8(kUiPageTarget) && (page == kPageMiniInv || page == kPageBuildView);
        }

        // FUN_003FA790 builds the frame's button set into a local vector (one
        // put per pad button, ZL's exactly once per frame). After ZL's put,
        // set the mod's private grapple button too. The InputMapper's own set
        // (FUN_001EDB74's direct put) is left alone.
        void Hooked_ButtonSetPut(u32 *set, u8 *index, u32 *value)
        {
            hook::FunctionHook::CallOriginal<void>(set, index, value);
            if (*index != kBtnZL || reinterpret_cast<u32>(set) == kActionSetLive)
                return;
            const u32 begin = set[0], end = set[1];
            if (!begin || end < begin || end - begin < (kBtnModGrapple + 1u) * 8u)
                return;
            const bool down = s_modGrappleDown;
            const float v = down ? 1.0f : 0.0f;
            u32 *e = reinterpret_cast<u32 *>(begin + kBtnModGrapple * 8u);
            e[0] = down ? 1u : 0u;                      // byte 0 = down (FUN_005337F0)
            e[1] = *reinterpret_cast<const u32 *>(&v);
        }

        // Game thread (pad hook), every frame: keep the game's own control
        // scheme in line with the ZL/ZR choices.
        //  - Grapple: point the scheme's grapple button (+5, stock A, read only
        //    at 0x41C9B8) at the mod's private button 0xA. ZL/ZR themselves are
        //    hidden from the game on the play pages, so nothing else needs
        //    unbinding.
        //  - Fallback if the button-set hook is missing (v0.5 behaviour, now
        //    on both play pages): grapple rides on the trigger's own bit, so
        //    unbind the stock page switches bound to it.
        void ApplySchemeOverrides(bool onPlayPage)
        {
            if ((U8(kSchemeGuard) & 1) == 0)
                return;
            const int zl = s_zlAction, zr = s_zrAction;
            const bool grapple = zl == kTrigGrapple || zr == kTrigGrapple;
            const bool viaSet = s_buttonSetHooked;
            s_ovGrapple.Set(grapple, viaSet ? kBtnModGrapple : zl == kTrigGrapple ? kBtnZL : kBtnZR);
            s_ovMiniInv.Set(!viaSet && onPlayPage && zl == kTrigGrapple, 0);
            s_ovInventory.Set(!viaSet && onPlayPage && zr == kTrigGrapple, 0);
        }

        float StickMagnitude(float &nx, float &ny)
        {
            nx = s_smX / kStickFull;
            ny = -s_smY / kStickFull;       // world +y is down
            float mag = nx * nx + ny * ny;
            mag = mag > 0.0f ? __builtin_sqrtf(mag) : 0.0f;
            return mag > 1.0f ? 1.0f : mag;
        }

        enum class HeldKind { Other, Tool, Places };

        HeldKind HeldItemKind(u32 player)
        {
            const s32 sel = *reinterpret_cast<volatile s8 *>(player + kPlayerSelected);
            if (sel < 0 || sel > 49)
                return HeldKind::Other;
            const u32 item = player + kPlayerInventory + sel * 0x84;
            if (U8(item + 0x5D) || U8(item + 0x5E) || U8(item + 0x5F))   // pick / axe / hammer power
                return HeldKind::Tool;
            if (S16(item + 0x2E) >= 0)                                    // places a tile
                return HeldKind::Places;
            return HeldKind::Other;
        }

        // ZL "cycle C-stick style" (game thread). Only Spring and Cursor exist:
        // Spring is the absolute style (push = point in the reach box).
        void CycleStyle()
        {
            s_cstickStyle = s_cstickStyle == kStyleCursor ? kStyleSpring : kStyleCursor;
            ResetHold();
            s_styleNotify = true;
        }

        bool Hooked_PadReadLatest(void *reader, u8 *status)
        {
            const bool ok = hook::FunctionHook::CallOriginal<bool>(reader, status);
            s_padCalls = s_padCalls + 1;
            s_gameX = *reinterpret_cast<s16 *>(status + 4);
            s_gameY = *reinterpret_cast<s16 *>(status + 6);
            s_padConnected = status[0x15];
            s_notConnectedFrames = s_padConnected ? 0 : s_notConnectedFrames + 1;
            MergeStick();

            // "In play": a world is drawing (the world draw ran since last
            // frame), the bottom screen is a play page (normal play or the
            // build view; not inventory, chest, pause or dialog) and there is
            // a local player.
            // ReadLatest runs once per game tick, not per drawn frame: below
            // 60 fps the game runs 2+ ticks per draw (v0.5.0 hardware log:
            // 60 pad reads/s at 26-36 fps). Count a world draw within the last
            // few ticks, or Y/B/grapple would drop out every other tick.
            const u32 draws = stereo::layers::WorldDrawCount();
            if (draws != s_lastWorldDraws)
                s_ticksSinceWorldDraw = 0;
            else if (s_ticksSinceWorldDraw < kWorldDrawGraceTicks)
                s_ticksSinceWorldDraw++;
            const bool inWorld = s_ticksSinceWorldDraw < kWorldDrawGraceTicks;
            s_lastWorldDraws = draws;
            const bool onPlayPage = UiOnPlayPage();
            const u32 player = LocalPlayer();
            const bool inPlay = inWorld && onPlayPage && player;

            // ZL / ZR. Every game input goes through this one status (the
            // only caller is FUN_003FA790, which maps held bits 0x4000 / 0x8000
            // to button indices 9 / 6). On hardware they can arrive from the
            // emulated Circle Pad Pro, the plain HID pad, or (added here) the
            // plugin's ir:rst session. A remapped trigger is taken out of the
            // status entirely on the play pages, so no stock control can see
            // it whatever the route, and the mod acts on the raw state.
            u32 &held = *reinterpret_cast<u32 *>(status + 8);
            u32 &down = *reinterpret_cast<u32 *>(status + 0xC);
            u32 &up   = *reinterpret_cast<u32 *>(status + 0x10);
            const bool physY = (held & kKeyY) != 0;     // the real Y, before any injected use

            // Auto-resume after the plugin menu (see s_menuFromPlay): A held
            // for 2 ticks, "down" on the first, only on the pause page.
            if (s_resumePress)
            {
                // Stop as soon as the pause page is gone (no A reaches play).
                if ((U8(kUiGuard) & 1) && U8(kUiPage) == kPagePause)
                {
                    held |= kKeyA;
                    if (s_resumePress == 2)
                        down |= kKeyA;
                    s_resumePress--;
                }
                else
                    s_resumePress = 0;
            }
            else if (s_resumeWindow)
            {
                s_resumeWindow = s_resumeWindow - 1;
                if ((U8(kUiGuard) & 1) && U8(kUiPage) == kPagePause && U8(kUiPageTarget) == kPagePause
                    && (held & kKeyA) == 0)
                {
                    s_resumeWindow = 0;
                    s_resumePress = 2;
                }
            }

            // D-pad walking: the D-pad becomes the circle pad (status +0/+2,
            // about +-105, up = +) while the circle pad itself is centred, and
            // the D-pad bits are hidden from the game (no D-pad cursor). Play
            // pages only: menus keep the D-pad.
            s_dpadWalking = false;
            if (s_dpadWalk && inPlay)
            {
                const u32 d = held & kKeyDpad;
                s16 &px = *reinterpret_cast<s16 *>(status + 0);
                s16 &py = *reinterpret_cast<s16 *>(status + 2);
                const bool padIdle = px * px + py * py < 20 * 20;
                if (d && padIdle)
                {
                    px = static_cast<s16>((d & 0x10 ? 105 : 0) - (d & 0x20 ? 105 : 0));   // right / left
                    py = static_cast<s16>((d & 0x40 ? 105 : 0) - (d & 0x80 ? 105 : 0));   // up / down
                    s_dpadWalking = true;
                }
                // Release edges pass: a D-pad pressed on a menu page and let
                // go in play still ends there.
                held &= ~kKeyDpad;
                down &= ~kKeyDpad;
            }

            // Stock Y: a Y press with the C-stick centred hands the aim back
            // to the game's smart cursor until Y is released (Spring: no
            // "keep the aimed tile" lock; Cursor: the crosshair hides).
            {
                float nx, ny;
                const bool stickIdle = StickMagnitude(nx, ny) <= kAimDeadzone;
                if (!physY)
                    s_yStockLatch = false;
                else if (!s_prevPhysY && inPlay && s_yButton == kYStock && s_cstickMode != kCstickOff && stickIdle)
                {
                    s_yStockLatch = true;
                    if (s_cstickStyle == kStyleCursor && s_holdPlaced)
                        s_holdHidden = true;
                }
                s_prevPhysY = physY;
            }

            const int act[2] = { s_zlAction, s_zrAction };
            const u32 bit[2] = { kKeyZL, kKeyZR };
            const bool remapAny = act[0] != kTrigDefault || act[1] != kTrigDefault;
            const u32 raw = held | (remapAny && s_irActive ? s_irKeys & (kKeyZL | kKeyZR) : 0u);
            const bool viaSet = s_buttonSetHooked;
            bool modGrapple = false;
            bool modUse = false;                        // use injected by a trigger or auto-use
            ApplySchemeOverrides(onPlayPage);
            for (int i = 0; i < 2; i++)
            {
                const bool rawHeld = (raw & bit[i]) != 0;
                const bool pressEdge = rawHeld && !s_trigPrevRaw[i];
                s_trigPrevRaw[i] = rawHeld;
                if (act[i] == kTrigDefault)
                {
                    if (rawHeld)
                        held |= bit[i];     // stock trigger, ir:rst included (as v0.5)
                    s_trigOwned[i] = false;
                    s_trigHeldFrames[i] = 0;
                    continue;
                }
                // A press made on a play page stays the mod's until it is
                // released, even if the page changes meanwhile (no stray stock
                // ZR edge closing the inventory a touch-open just opened).
                s_trigOwned[i] = rawHeld && (onPlayPage || s_trigOwned[i]);
                const bool hide = onPlayPage || s_trigOwned[i];
                // Fallback only: grapple needs the trigger's own bit.
                const bool bitGrapple = act[i] == kTrigGrapple && !viaSet;
                if (hide && !bitGrapple)
                {
                    held &= ~bit[i];
                    down &= ~bit[i];
                    up   &= ~bit[i];
                }
                else if (rawHeld)
                    held |= bit[i];         // menus keep the stock trigger (ir:rst if the game lost it)

                // Cycle the C-stick style: one step per press made in play.
                if (act[i] == kTrigStyle && pressEdge && inPlay)
                    CycleStyle();

                if (inPlay && rawHeld)
                {
                    s_trigHeldFrames[i]++;
                    // Use and jump ride on Y / B so the game's own rules apply
                    // (cursor follow while using, auto-reuse, jump height...).
                    if (act[i] == kTrigUse)  { held |= kKeyY; modUse = true; }
                    if (act[i] == kTrigJump) held |= kKeyB;
                    // The game throws a hook only on a fresh press (0x41C9CC)
                    // and only while no hook of yours is out (FUN_00426268,
                    // P+0x65DC). Holding the grapple trigger re-presses it
                    // every few frames, so a missed or released hook is
                    // thrown again without letting go.
                    if (act[i] == kTrigGrapple)
                    {
                        const bool press = s_trigHeldFrames[i] % kRegrappleFrames != 0;
                        if (viaSet)
                            modGrapple = modGrapple || press;
                        else if (!press)
                            held &= ~bit[i];
                    }
                }
                else
                    s_trigHeldFrames[i] = 0;
            }
            s_modGrappleDown = modGrapple;

            // Cursor style: move the held aim (game thread, once per tick).
            // Reset on entering a world or a new local player, or on request.
            s_padTick++;
            const bool hasWorld = inWorld && player;
            if ((hasWorld && (!s_holdWasInWorld || player != s_holdPlayer)) || s_holdResetReq)
            {
                ResetHold();
                s_holdResetReq = false;
            }
            s_holdWasInWorld = hasWorld;
            s_holdPlayer = player;
            s_dpadHeld = inPlay && (held & kKeyDpad) != 0;
            float snx, sny;
            const float smag = StickMagnitude(snx, sny);
            if (s_cstickStyle == kStyleCursor && s_cstickMode != kCstickOff && inPlay)
            {
                if (smag > kHoldDeadzone)
                {
                    // Acceleration curve: a small push creeps (a few px/s) for
                    // precision, a full push runs at the chosen speed.
                    const float t   = (smag - kHoldDeadzone) / (1.0f - kHoldDeadzone);
                    const float c   = t * t * (0.35f + 0.65f * t);
                    const int   si  = s_cursorSpeed;
                    const float v   = kHoldSpeed[si < 0 || si > 2 ? 1 : si] * c * kTickDt;
                    const float len = __builtin_sqrtf(snx * snx + sny * sny);
                    s_holdX += snx / len * v;
                    s_holdY += sny / len * v;
                    ClampHold();
                    s_holdPlaced = true;
                    s_holdHidden = false;       // aiming again: the crosshair comes back where it was
                }
            }

            const HeldKind kind = inPlay ? HeldItemKind(player) : HeldKind::Other;
            // Auto-tunnel: a tool on the C-stick steers the game's smart dig
            // (Cursor::Update runs after this read, in the same tick). Precise
            // never tunnels: it mines the aimed tile only.
            const bool tunnelTool = s_cstickMode != kCstickOff && inPlay && s_cstickStyle != kStyleCursor
                                 && s_toolMining == kToolTunnel && s_smartHooked && kind == HeldKind::Tool;
            bool autoUse = false;
            if (s_cstickMode != kCstickOff)
            {
                // The stock C-stick only pans the camera; aiming replaces that.
                *reinterpret_cast<s16 *>(status + 4) = 0;
                *reinterpret_cast<s16 *>(status + 6) = 0;

                // Twin-stick: a hard push holds Y for the game, only in play,
                // and only for weapons and tools: never places blocks or uses
                // up consumables. Auto-tunnel tools start at a smaller push.
                float at = tunnelTool ? kAutoUseTunnelAt : kAutoUseAt;
                if (s_autoUsing)
                    at -= kAutoUseHyst;
                if (s_cstickMode == kCstickAimAuto && inPlay && smag > at)
                {
                    const u32 item = player + kPlayerInventory
                                   + *reinterpret_cast<volatile s8 *>(player + kPlayerSelected) * 0x84;
                    const bool consumable = (U8(item + 0x75) & 0x20) != 0;   // tested at 0x441F1C
                    if (kind == HeldKind::Tool || (kind == HeldKind::Other && !consumable))
                    {
                        held |= kKeyY;
                        modUse = true;
                        autoUse = true;
                    }
                }
            }
            s_autoUsing = autoUse;

            bool tunnel = false;
            if (tunnelTool && smag > kAimDeadzone)
            {
                const float len = __builtin_sqrtf(snx * snx + sny * sny);
                s_tunnelDX = snx / len;
                s_tunnelDY = sny / len;
                tunnel = true;
            }
            s_tunnelActive = tunnel;

            // "Use at the C-stick aim": a trigger / auto-use, or a Y press that
            // isn't the stock-Y kind.
            const bool useForAim = modUse || (physY && !s_yStockLatch);
            s_useHeld = inPlay && useForAim;

            // Cursor auto-hide (Cursor style): walking with the C-stick idle,
            // or (option) 3 s with no C-stick. Never while using at the aim.
            if (smag > kHoldDeadzone)
                s_cstickIdleTicks = 0;
            else if (s_cstickIdleTicks < 0xFFFF)
                s_cstickIdleTicks++;
            if (s_cstickStyle == kStyleCursor && inPlay && s_holdPlaced && !s_holdHidden && !useForAim)
            {
                const int h = s_cursorHide;
                if ((h != kHideNever && s_cstickIdleTicks >= kHideGraceTicks && CirclePadMoving())
                    || (h == kHideWalkingOrIdle && s_cstickIdleTicks >= kHideIdleTicks))
                    s_holdHidden = true;
            }
            return ok;
        }

        // "Circle pad = interactions only": the smart cursor still finds doors,
        // chests and tables for A, but a pickaxe or block swung with nothing
        // aimed (no C-stick, D-pad cursor or touch) hits nothing. ItemCheck
        // copies C+5 into the player's "tool target valid" flag and PlaceThing
        // gives up when the aim has no cursor, so hide C+5 for ItemCheck only.
        void Hooked_ItemCheck(u32 player, u32 index)
        {
            u8 *cursor = reinterpret_cast<u8 *>(U32(kCursorPtr));
            bool hidden = false;
            u8 saved = 0;
            if (s_padInteractOnly && cursor && player && player == LocalPlayer()
                && cursor[4] == 0 && cursor[0x30] == 0 && !s_aimWasActive && !s_tunnelActive
                && HeldItemKind(player) != HeldKind::Other)
            {
                saved = cursor[5];
                cursor[5] = 0;
                hidden = true;
            }
            hook::FunctionHook::CallOriginal<void>(player, index);
            if (hidden && cursor[4] == 0)
                cursor[5] = saved;
        }

        // Longest ray from the player centre along unit (dx, dy) that stays in
        // a reach box of x +-side, y -up..+down.
        float RayMax(float dx, float dy, float side, float up, float down)
        {
            const float rx = dx > 1e-4f || dx < -1e-4f ? side / (dx < 0 ? -dx : dx) : 1e9f;
            const float ry = dy > 1e-4f || dy < -1e-4f ? (dy < 0 ? up : down) / (dy < 0 ? -dy : dy) : 1e9f;
            return rx < ry ? rx : ry;
        }

        // Blocks only stick to an empty tile next to another tile
        // (WorldGen.PlaceTile), so aiming into the air or into the ground
        // placed nothing. For a placeable item, walk the aim ray and take the
        // placeable tile nearest to distance r. Nothing suitable: keep (ax, ay)
        // (walls etc.).
        void SnapToPlaceable(u32 player, float cx, float cy, float dx, float dy, float r, float rmax,
                             float &ax, float &ay)
        {
            const float px = F32(player + kPlayerPosX), py = F32(player + kPlayerPosY);
            const float pw = *reinterpret_cast<volatile u16 *>(player + kPlayerWidth);
            const float ph = *reinterpret_cast<volatile u16 *>(player + kPlayerHeight);
            float bestErr = 1e9f;
            int lastX = -99999, lastY = -99999;
            for (float rr = 8.0f; rr <= rmax; rr += 4.0f)
            {
                const int tx = static_cast<int>(cx + dx * rr) >> 4;
                const int ty = static_cast<int>(cy + dy * rr) >> 4;
                if (tx == lastX && ty == lastY)
                    continue;
                lastX = tx;
                lastY = ty;
                if (TileState(tx, ty) != 0)
                    continue;
                if (TileState(tx - 1, ty) != 1 && TileState(tx + 1, ty) != 1
                    && TileState(tx, ty - 1) != 1 && TileState(tx, ty + 1) != 1)
                    continue;
                const float l = tx * 16.0f, tp = ty * 16.0f;     // not inside the player
                if (l < px + pw && l + 16.0f > px && tp < py + ph && tp + 16.0f > py)
                    continue;
                const float err = rr > r ? rr - r : r - rr;
                if (err < bestErr)
                {
                    bestErr = err;
                    ax = l + 8.0f;
                    ay = tp + 8.0f;
                }
            }
        }

        // Spring (absolute) style: the deflection maps to a point inside the
        // reach box; released = no aim. Returns false when the stick is idle.
        bool SpringAim(u32 player, float cx, float cy, float &ax, float &ay)
        {
            float nx, ny;
            const float mag = StickMagnitude(nx, ny);
            if (mag <= kAimDeadzone)
                return false;
            const float len = __builtin_sqrtf(nx * nx + ny * ny);
            const float dx = nx / len, dy = ny / len;
            // Stay inside the game's own reach box around the player
            // (dx ±128, dy -104..+88 px) so mining/placing stay legal; a
            // small push aims near the player, a full push at the edge.
            const float rmax = RayMax(dx, dy, 128.0f, 104.0f, 88.0f);
            // Sensitivity: blend linear with a square so a small push
            // moves the aim less (slope = gain) but a full push still
            // reaches the edge of the reach box.
            const float t  = (mag - kAimDeadzone) / (1.0f - kAimDeadzone);
            const int   si = s_sensitivity;
            const float g  = kSensGain[si < 0 || si > 2 ? 1 : si];
            const float r  = 16.0f + (rmax - 16.0f) * (g * t + (1.0f - g) * t * t);
            ax = cx + dx * r;
            ay = cy + dy * r;
            if (HeldItemKind(player) == HeldKind::Places)
                SnapToPlaceable(player, cx, cy, dx, dy, r, rmax, ax, ay);
            return true;
        }

        // Cursor (holds position) style. The stick moves s_holdX/Y in the pad
        // hook; here the D-pad and touch, which move the game's own cursor,
        // are adopted as the new held point (first call of the tick only:
        // later calls see our own write). Returns false while parked.
        bool HoldAim(u32 player, const u8 *cursor, float cx, float cy, float &ax, float &ay)
        {
            const bool firstThisTick = s_holdSeenTick != s_padTick;
            s_holdSeenTick = s_padTick;
            const bool touch = cursor[0x30] != 0;
            if (firstThisTick && (touch || (s_dpadHeld && cursor[4] == 1)))
            {
                s_holdX = *reinterpret_cast<const float *>(cursor + 0x08) - cx;
                s_holdY = *reinterpret_cast<const float *>(cursor + 0x0C) - cy;
                ClampHold();
                s_holdPlaced = true;
                s_holdHidden = false;
            }
            if (!s_holdPlaced || s_holdHidden)
                return false;       // parked, or hidden (offset kept for the next push)
            ClampHold();        // the reach box can shrink while held (Long reach turned off)
            ax = cx + s_holdX;
            ay = cy + s_holdY;
            // Same placement help as Spring, along the player -> cursor ray
            // and nearest the held distance (touch keeps the stock spot).
            const float len = __builtin_sqrtf(s_holdX * s_holdX + s_holdY * s_holdY);
            if (!touch && len > 4.0f && HeldItemKind(player) == HeldKind::Places)
            {
                const float dx = s_holdX / len, dy = s_holdY / len;
                float side, up, down;
                ReachBox(side, up, down);
                SnapToPlaceable(player, cx, cy, dx, dy, len, RayMax(dx, dy, side, up, down), ax, ay);
            }
            return true;
        }

        void Hooked_GetAimPoint(float *out, u8 *cursor)
        {
            s_aimCalls = s_aimCalls + 1;
            const u32 player = LocalPlayer();
            bool aimed = false;
            if (s_tunnelActive && cursor)
            {
                // Auto-tunnel: leave the cursor to the smart cursor (mode 0),
                // which Hooked_SmartCursor steers with the C-stick. Touch wins.
                if (cursor[0x30] == 0)
                    cursor[4] = 0;
                s_aimWasActive = false;
            }
            else if (s_cstickMode != kCstickOff && player && cursor)
            {
                const float cx = F32(player + kPlayerPosX) + *reinterpret_cast<volatile u16 *>(player + kPlayerWidth) * 0.5f;
                const float cy = F32(player + kPlayerPosY) + *reinterpret_cast<volatile u16 *>(player + kPlayerHeight) * 0.5f;
                float ax = 0, ay = 0;
                aimed = s_cstickStyle == kStyleCursor ? HoldAim(player, cursor, cx, cy, ax, ay)
                                                      : SpringAim(player, cx, cy, ax, ay);
                if (aimed)
                {
                    *reinterpret_cast<float *>(cursor + 0x08) = ax;
                    *reinterpret_cast<float *>(cursor + 0x0C) = ay;
                    cursor[4] = 1;      // manual cursor (Cursor::Update keeps C+8 in mode 1)
                    cursor[5] = 1;      // position valid: GetAimPoint and Cursor::Draw use C+8
                    s_aimWasActive = true;
                }
            }
            if (!aimed && s_aimWasActive && cursor && !s_useHeld)
            {
                // While use (Y/ZR) is still held the aimed tile stays put: the
                // game's manual cursor follows the player while use is held.
                cursor[4] = 0;      // hand back to the smart cursor / X lock
                cursor[5] = 0;
                s_aimWasActive = false;
            }
            hook::FunctionHook::CallOriginal<void>(out, cursor);
        }

        // Smart cursor FUN_00263B98(out, C, dir), called from Cursor::Update
        // mode 0 (FUN_002649E4) with dir = C+0x10 (circle pad, or facing).
        // With a tool it runs the game's smart dig FUN_0041F500(out, P, &n,
        // dir): three parallel rays from the player along dir (the 3-tall
        // tunnel), limited by the tool's own reach (item tileBoost + P+0x6560),
        // skipping tiles a grapple hook is on. Auto-tunnel swaps in the
        // C-stick direction (a local copy; C+0x10 itself is untouched).
        hook::FunctionHook s_smartCursorHook;

        void Hooked_SmartCursor(float *out, u8 *cursor, float *dir)
        {
            if (s_tunnelActive && dir)
            {
                float d[2] = { s_tunnelDX, s_tunnelDY };
                hook::FunctionHook::CallOriginal<void>(out, cursor, d);
                return;
            }
            hook::FunctionHook::CallOriginal<void>(out, cursor, dir);
        }

        void SetFlag(u32 addr, bool on, bool &prev)
        {
            if (on)
                U8(addr) = 1;
            else if (prev)
                U8(addr) = 0;   // only clear what we set, once
            prev = on;
        }

        // ---- menu ----
        // Toggle entries carry a no-op game function (so CTRPF shows a checkbox);
        // PollMenu() reads their state every frame on the plugin thread, so
        // turning something OFF is always seen and undone.
        void NoOp(MenuEntry *) {}

        MenuEntry *e_zoomHotkey, *e_consoleHotkey;
        MenuEntry *e_godmode, *e_noSpawns, *e_fullbright, *e_infBuckets, *e_debugTools;
        MenuEntry *e_infItems, *e_noFall, *e_instantMine, *e_reach, *e_flight, *e_freezeTime;
        MenuEntry *e_padInteract, *e_cstickDebug, *e_dpadWalk;

        const char *const kCstickModeNames[] = { "Off (stock camera pan)", "Aim only (Y uses)",
                                                 "Aim + auto-use on hard push" };

        void CstickModeMenuFunc(MenuEntry *)
        {
            Keyboard kb(Utils::Format("C-stick aiming. Current: %s", kCstickModeNames[s_cstickMode]),
                        { kCstickModeNames[0], kCstickModeNames[1], kCstickModeNames[2] });
            const int choice = kb.Open();
            if (choice >= 0 && choice < 3)
            {
                s_cstickMode = choice;
                s_holdResetReq = true;
            }
        }

        const char *const kCstickStyleNames[] = { "Spring (absolute)", "Cursor (holds position)" };
        const char *const kCstickStyleShort[] = { "Spring", "Cursor" };
        const char *const kCursorSpeedNames[] = { "Slow", "Normal", "Fast" };

        void CstickStyleMenuFunc(MenuEntry *)
        {
            Keyboard kb(Utils::Format("C-stick STYLE (how the aim moves). Current: %s\n\n"
                                      "Spring: the push sets the aim (absolute), letting\n"
                                      "  go returns to the auto-cursor.\n"
                                      "Cursor: the stick moves the aim like a mouse\n"
                                      "  and it stays where you leave it (it hides\n"
                                      "  while you walk). Tools mine that one spot.\n"
                                      "In play: ZL switches (New 3DS preset).\n"
                                      "Not the same as \"C-stick tool mode\".",
                                      kCstickStyleNames[s_cstickStyle == kStyleCursor]),
                        { kCstickStyleNames[0], kCstickStyleNames[1] });
            const int choice = kb.Open();
            if (choice >= 0 && choice < 2 && choice != s_cstickStyle)
            {
                s_cstickStyle = choice;
                s_holdResetReq = true;
                perf::Log(std::string("C-stick style: ") + kCstickStyleNames[choice]);
            }
        }

        void CursorSpeedMenuFunc(MenuEntry *)
        {
            Keyboard kb(Utils::Format("Cursor speed (C-stick style \"Cursor\"). Current: %s",
                                      kCursorSpeedNames[s_cursorSpeed]),
                        { kCursorSpeedNames[0], kCursorSpeedNames[1], kCursorSpeedNames[2] });
            const int choice = kb.Open();
            if (choice >= 0 && choice < 3)
                s_cursorSpeed = choice;
        }

        const char *const kCursorHideNames[] = { "When walking", "When walking or after 3 s", "Never" };
        const char *const kYButtonNames[]    = { "Stock (smart cursor)", "Uses the C-stick aim" };
        const char *const kToolMiningNames[] = { "Auto-tunnel (smart dig)", "Precise (one block, the aimed tile)" };

        void CursorHideMenuFunc(MenuEntry *)
        {
            Keyboard kb(Utils::Format("Hide the Cursor-style crosshair. Current: %s\n\n"
                                      "It comes back where it was on the next C-stick\n"
                                      "push, D-pad step or touch. Never hides while\n"
                                      "you use an item at it.", kCursorHideNames[s_cursorHide]),
                        { kCursorHideNames[0], kCursorHideNames[1], kCursorHideNames[2] });
            const int choice = kb.Open();
            if (choice >= 0 && choice < 3)
                s_cursorHide = choice;
        }

        void YButtonMenuFunc(MenuEntry *)
        {
            Keyboard kb(Utils::Format("Y button. Current: %s\n\n"
                                      "Stock: Y uses the item at the game's smart\n"
                                      "  cursor whenever the C-stick is centred.\n"
                                      "C-stick aim: Y uses the item at the C-stick\n"
                                      "  aim, like ZR (v0.5.2).", kYButtonNames[s_yButton]),
                        { kYButtonNames[0], kYButtonNames[1] });
            const int choice = kb.Open();
            if (choice >= 0 && choice < 2)
                s_yButton = choice;
        }

        void ToolMiningMenuFunc(MenuEntry *)
        {
            Keyboard kb(Utils::Format("C-stick TOOL MODE (pickaxe, axe, hammer;\n"
                                      "Spring style). Current: %s\n\n"
                                      "Auto-tunnel: push the C-stick towards the\n"
                                      "  rock (about half way); the game's smart dig\n"
                                      "  mines a tunnel your height, block after\n"
                                      "  block, within the tool's own reach.\n"
                                      "Precise: mine only the aimed tile. Never\n"
                                      "  tunnels.\n"
                                      "Cursor style always mines its held spot.\n"
                                      "Not the same as \"C-stick style\" (ZL).",
                                      kToolMiningNames[s_toolMining]),
                        { kToolMiningNames[0], kToolMiningNames[1] });
            const int choice = kb.Open();
            if (choice >= 0 && choice < 2)
                s_toolMining = choice;
        }

        // ---- control presets ----
        struct PresetDef
        {
            const char *name;
            int  cstick, zl, zr, sens, smooth;
            bool interactOnly;
        };
        const PresetDef kPresets[kPresetCount] = {
            // v0.5.4: ZL cycles the C-stick style; grapple is back on the
            // game's own button (A) in this preset.
            { "New 3DS (C-stick aim)",     kCstickAimAuto, kTrigStyle,   kTrigUse,     1, 2, false },
            // Old 3DS: no ZL/ZR without a Circle Pad Pro, so grapple stays on A
            // (moving it to ZL would leave no grapple button at all).
            { "Old 3DS (D-pad + touch)",   kCstickOff,     kTrigDefault, kTrigDefault, 1, 2, false },
            { "Stock (game controls)",     kCstickOff,     kTrigDefault, kTrigDefault, 1, 2, false },
            { "Lefty (ZL use, ZR grapple)",kCstickAimAuto, kTrigUse,     kTrigGrapple, 1, 2, false },
            // The v0.5.3 New 3DS layout.
            { "New 3DS, ZL grapple",       kCstickAimAuto, kTrigGrapple, kTrigUse,     1, 2, false },
        };
        const char *const kTrigNames[] = { "Off (game default)", "Grapple", "Jump", "Use item / mine / place",
                                           "Switch C-stick style" };
        const char *const kSensNames[] = { "Low", "Normal", "High" };
        const char *const kSmoothNames[] = { "Off", "Light", "Medium", "Strong" };

        void ApplyPreset(int p)
        {
            if (p < 0 || p >= kPresetCount)
                return;
            const PresetDef &d = kPresets[p];
            s_cstickMode  = d.cstick;
            s_zlAction    = d.zl;
            s_zrAction    = d.zr;
            s_sensitivity = d.sens;
            s_smoothing   = d.smooth;
            if (e_padInteract)
            {
                if (d.interactOnly) e_padInteract->Enable();
                else                e_padInteract->Disable();
            }
            s_padInteractOnly = d.interactOnly;
            s_preset = p;
        }

        // APT_CheckNew3DS (libctru), falling back to CTRPF's own check.
        bool DetectNew3DS()
        {
            bool isNew = false;
            if (R_SUCCEEDED(APT_CheckNew3DS(&isNew)))
                return isNew;
            return System::IsNew3DS();
        }

        void PresetMenuFunc(MenuEntry *)
        {
            const char *cur = s_preset >= 0 && s_preset < kPresetCount ? kPresets[s_preset].name : "custom";
            Keyboard kb(Utils::Format("Control preset. Current: %s\n\n"
                                      "New 3DS: C-stick aims, ZR uses, ZL switches\n"
                                      "  the C-stick style, A grapples (stock).\n"
                                      "Old 3DS: D-pad cursor + touch, A grapples\n"
                                      "  (Circle Pad Pro: set ZL/ZR below).\n"
                                      "Stock: the game's own controls.\n"
                                      "Lefty: ZL uses, ZR grapples.\n"
                                      "New 3DS, ZL grapple: the v0.5.3 layout.", cur),
                        { kPresets[0].name, kPresets[1].name, kPresets[2].name, kPresets[3].name,
                          kPresets[4].name });
            const int choice = kb.Open();
            if (choice >= 0 && choice < kPresetCount)
            {
                ApplyPreset(choice);
                perf::Log(std::string("control preset: ") + kPresets[choice].name);
            }
        }

        int PickTrigger(const char *button, int current)
        {
            Keyboard kb(Utils::Format("%s button (in play only; menus keep the\n"
                                      "game's own %s). Current: %s", button, button, kTrigNames[current]),
                        { kTrigNames[1], kTrigNames[2], kTrigNames[3], kTrigNames[4], kTrigNames[0] });
            const int choice = kb.Open();
            static const int kMap[] = { kTrigGrapple, kTrigJump, kTrigUse, kTrigStyle, kTrigDefault };
            return choice >= 0 && choice < 5 ? kMap[choice] : -1;
        }

        void ZlMenuFunc(MenuEntry *)
        {
            const int a = PickTrigger("ZL", s_zlAction);
            if (a < 0)
                return;
            if (a == kTrigGrapple && s_zrAction == kTrigGrapple)
                s_zrAction = kTrigDefault;      // one grapple button only
            if (a == kTrigStyle && s_zrAction == kTrigStyle)
                s_zrAction = kTrigDefault;
            s_zlAction = a;
            s_preset = -2;                      // custom
        }

        void ZrMenuFunc(MenuEntry *)
        {
            const int a = PickTrigger("ZR", s_zrAction);
            if (a < 0)
                return;
            if (a == kTrigGrapple && s_zlAction == kTrigGrapple)
                s_zlAction = kTrigDefault;
            if (a == kTrigStyle && s_zlAction == kTrigStyle)
                s_zlAction = kTrigDefault;
            s_zrAction = a;
            s_preset = -2;
        }

        void SensMenuFunc(MenuEntry *)
        {
            Keyboard kb(Utils::Format("C-stick aim sensitivity. Current: %s", kSensNames[s_sensitivity]),
                        { kSensNames[0], kSensNames[1], kSensNames[2] });
            const int choice = kb.Open();
            if (choice >= 0 && choice < 3)
                s_sensitivity = choice;
        }

        void SmoothMenuFunc(MenuEntry *)
        {
            Keyboard kb(Utils::Format("C-stick smoothing. Current: %s", kSmoothNames[s_smoothing]),
                        { kSmoothNames[0], kSmoothNames[1], kSmoothNames[2], kSmoothNames[3] });
            const int choice = kb.Open();
            if (choice >= 0 && choice < kSmoothLevels)
                s_smoothing = choice;
        }
        bool s_appliedInfItems = false, s_appliedNoFall = false, s_appliedMine = false, s_appliedReach = false;

        template <size_t N>
        void SyncPatch(MenuEntry *e, const Patch (&list)[N], bool &applied, const char *name)
        {
            const bool on = e && e->IsActivated();
            if (on == applied)
                return;
            const bool ok = ApplyPatches(list, on);
            applied = on;
            perf::Log(std::string(name) + (on ? " on" : " off") + (ok ? "" : " (PATCH MISMATCH)"));
        }

        void CheatConsoleMenuFunc(MenuEntry *)
        {
            if ((U8(kGsmGuard) & 1) == 0)
            {
                MessageBox("The game isn't ready yet.")();
                return;
            }
            U8(kCheatKeyboard) = 1;
            MessageBox("Cheat console",
                       "Close this menu (B) and the game's keyboard opens.\n"
                       "Commands: item <name|id>, godmode, nospawns, light,\n"
                       "dawn/noon/dusk/midnight, meteor, allbosses, map,\n"
                       "boost1/boost2/boost3/boostgod, hard, rain, ...")();
        }

        // ---- fake OLED colour booster ----
        // The display controller runs every output pixel through a 256-entry
        // per-channel lookup table on its way to the LCD (the same table
        // Rosalina's screen filters use): index at FB setup +0x80, entries
        // (0x00BBGGRR, auto-increment) at +0x84. Top FB setup 0x10400400,
        // bottom 0x10400500; Luma maps physical I/O at PA | 0x80000000.
        // Per channel only, so no true saturation: crush near-blacks to 0
        // (the OLED look) and an S-curve for contrast, which reads as punchier
        // colour. The table is written on change, and again after a gap of
        // more than 0.5 s between polls (HOME / sleep may reset it).
        constexpr u32 kLutIndexTop = 0x10400480, kLutElemTop = 0x10400484;
        constexpr u32 kLutIndexBot = 0x10400580, kLutElemBot = 0x10400584;
        inline volatile u32 &IoReg(u32 pa) { return *reinterpret_cast<volatile u32 *>(pa | 0x80000000u); }

        const char *const kColorModeNames[] = { "Normal", "OLED (deep blacks)", "OLED vivid (strong contrast)" };
        volatile int s_colorMode = 0;
        int          s_colorApplied = 0;    // leave the table alone until the user picks a mode
        u64          s_colorTimer = 0;    // system tick of the previous poll

        u8 BoostCurve(int i, int mode)
        {
            if (mode == 0)
                return static_cast<u8>(i);
            const float black = mode == 1 ? 0.05f : 0.08f;   // crushed to true black below this
            const float s     = mode == 1 ? 0.35f : 0.65f;   // S-curve strength
            float x = (i / 255.0f - black) / (1.0f - black);
            x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
            const float smooth = x * x * (3.0f - 2.0f * x);
            x = x + (smooth - x) * s;
            return static_cast<u8>(x * 255.0f + 0.5f);
        }

        void WriteColorLut(int mode)
        {
            const u32 index[] = { kLutIndexTop, kLutIndexBot };
            const u32 elem[]  = { kLutElemTop, kLutElemBot };
            for (int scr = 0; scr < 2; scr++)
            {
                IoReg(index[scr]) = 0;
                for (int i = 0; i < 256; i++)
                {
                    const u32 v = BoostCurve(i, mode);
                    IoReg(elem[scr]) = v | v << 8 | v << 16;
                }
            }
        }

        void ColorModeMenuFunc(MenuEntry *)
        {
            Keyboard kb(Utils::Format("Fake OLED colour booster. Current: %s", kColorModeNames[s_colorMode]),
                        { kColorModeNames[0], kColorModeNames[1], kColorModeNames[2] });
            const int choice = kb.Open();
            if (choice >= 0 && choice < 3)
                s_colorMode = choice;
        }

        void NoonMenuFunc(MenuEntry *)
        {
            F32(kTime) = 27000.0f;
            U8(kDayTime) = 1;
            OSD::Notify("Time set to noon");
        }
    }

    // Plugin thread, every frame (registered in BuildMenu)
    void PollMenu()
    {
        // Colour booster: write on change, and re-apply after a pause in polling
        // (HOME menu or sleep can reset the table). v0.5.2 rewrote the table every
        // second while on; rare full-screen white/pink flashes were seen only with
        // the booster on, so no more blind periodic rewrites during scan-out.
        const u64 now = svcGetSystemTick();
        const bool resumed = s_colorTimer != 0 && now - s_colorTimer > SYSCLOCK_ARM11 / 2;
        s_colorTimer = now;
        if (s_colorMode != s_colorApplied || (s_colorMode != 0 && resumed))
        {
            WriteColorLut(s_colorMode);
            if (s_colorMode != s_colorApplied)
                perf::Log(std::string("colour mode: ") + kColorModeNames[s_colorMode]);
            s_colorApplied = s_colorMode;
        }

        if (e_zoomHotkey && e_zoomHotkey->IsActivated() && e_zoomHotkey->Hotkeys[0].IsPressed())
        {
            s_zoom2x = !s_zoom2x;
            s_zoomDirty = true;
        }
        if (e_consoleHotkey && e_consoleHotkey->IsActivated() && e_consoleHotkey->Hotkeys[0].IsPressed()
            && (U8(kGsmGuard) & 1))
            U8(kCheatKeyboard) = 1;

        s_godmode    = e_godmode->IsActivated();
        s_noSpawns   = e_noSpawns->IsActivated();
        s_fullbright = e_fullbright->IsActivated();
        s_infBuckets = e_infBuckets->IsActivated();
        s_debugTools = e_debugTools->IsActivated();
        s_flight     = e_flight->IsActivated();
        s_freezeTime = e_freezeTime->IsActivated();
        s_padInteractOnly = e_padInteract->IsActivated();
        s_dpadWalk = e_dpadWalk && e_dpadWalk->IsActivated();

        // OSD for the in-game style toggle (formatted here, not on the game thread)
        if (s_styleNotify)
        {
            s_styleNotify = false;
            const bool cursor = s_cstickStyle == kStyleCursor;
            OSD::Notify(Utils::Format("C-stick style: %s%s", kCstickStyleShort[cursor ? 1 : 0],
                                      s_cstickMode == kCstickOff ? " (aiming is off)" : ""));
            perf::Log(std::string("C-stick style (button): ") + kCstickStyleNames[cursor ? 1 : 0]);
        }

        // ir:rst: on hardware (v0.4.1 playtest) the game's emulated Circle Pad
        // Pro reports "connected" but its C-stick stays 0,0, so read the stick
        // from the system's ir:rst service as well, starting 3 s after boot.
        // MergeStick() still prefers the game's value whenever it is non-zero.
        static u32 s_polls = 0;
        s_polls++;

#ifdef T3D_O3DS
        if (s_polls == 31)
        {
            const bool isNew = DetectNew3DS();
            perf::Log(Utils::Format("console: %s 3DS, Old 3DS build: preset %s (fixed)%s", isNew ? "New" : "Old",
                                    kPresets[kPresetOld3DS].name,
                                    isNew ? " - NOTE: this console should use the New 3DS build" : ""));
        }
#else
        // First run: choose the preset for this console (New 3DS: C-stick,
        // ZL, ZR; Old 3DS: D-pad cursor + touch).
        if (s_preset == -1 && s_polls > 30)
        {
            const bool isNew = DetectNew3DS();
            ApplyPreset(isNew ? kPresetNew3DS : kPresetOld3DS);
            perf::Log(Utils::Format("console: %s 3DS -> preset %s", isNew ? "New" : "Old",
                                    kPresets[s_preset].name));
        }

        const bool wantIr = s_cstickMode != kCstickOff || s_zlAction != kTrigDefault || s_zrAction != kTrigDefault;
        if (!s_irActive && wantIr && s_polls > 180)
        {
            const Result rc = irrstInit();
            s_irActive = R_SUCCEEDED(rc);
            perf::Log(Utils::Format("C-stick: game extra pad %s (pad reads %lu), ir:rst fallback %s (0x%08lX)",
                                    s_padConnected ? "connected" : "NOT connected", s_padCalls,
                                    s_irActive ? "started" : "FAILED", rc));
        }
        if (s_irActive)
        {
            irrstScanInput();
            circlePosition pos;
            irrstCstickRead(&pos);
            s_irX = ScaleIr(pos.dx);
            s_irY = ScaleIr(pos.dy);
            s_irKeys = irrstKeysHeld() & (KEY_ZL | KEY_ZR);
            if (s_padCalls == 0)
                MergeStick();   // no game-side read to merge in
        }
#endif

        SyncPatch(e_infItems,   kInfiniteItems, s_appliedInfItems, "infinite items");
        SyncPatch(e_noFall,     kNoFallDamage,  s_appliedNoFall,   "no fall damage");
        SyncPatch(e_instantMine,kInstantMine,   s_appliedMine,     "instant mine");

        const bool reach = e_reach->IsActivated();
        if (reach != s_appliedReach)
        {
            mem::WriteFloat(kReachX, reach ? 512.0f : 128.0f);
            mem::WriteFloat(kReachY, reach ? 512.0f : 96.0f);
            s_appliedReach = reach;
        }
    }

    void Install()
    {
        // Show the hidden "dev" group in Settings (nothing else changes)
        if (mem::Replace32(kDevButtonBranch, 0x1A00000E, 0xEA00000E))
            perf::Log("dev settings button unlocked");

        if (s_flightHook.Install(kFnPlayerMove, reinterpret_cast<void *>(Hooked_PlayerMove), 0xE92D47F0)
            != hook::Result::Ok)
            perf::Log("flight hook NOT installed");
        if (s_padHook.Install(kFnPadReadLatest, reinterpret_cast<void *>(Hooked_PadReadLatest), 0xE92D4070)
            != hook::Result::Ok)
            perf::Log("C-stick read hook NOT installed");
#ifdef T3D_O3DS
        // Old 3DS build: no C-stick aim, smart-cursor steering or private
        // grapple button. Those hooks run many times per tick and would only
        // pass through here, so they are not installed at all.
        if (s_itemCheckHook.Install(kFnItemCheck, reinterpret_cast<void *>(Hooked_ItemCheck), 0xE92D4FF3)
            != hook::Result::Ok)
            perf::Log("item-check hook NOT installed");
        perf::Log("Old 3DS build: C-stick, ZL/ZR remap and ir:rst are not installed");
#else
        if (s_aimHook.Install(kFnGetAimPoint, reinterpret_cast<void *>(Hooked_GetAimPoint), 0xE92D4030)
            != hook::Result::Ok)
            perf::Log("aim hook NOT installed");
        s_smartHooked = s_smartCursorHook.Install(kFnSmartCursor, reinterpret_cast<void *>(Hooked_SmartCursor),
                                                  0xE92D41F0) == hook::Result::Ok;
        if (!s_smartHooked)
            perf::Log("smart-cursor hook NOT installed: C-stick auto-tunnel off");
        if (s_itemCheckHook.Install(kFnItemCheck, reinterpret_cast<void *>(Hooked_ItemCheck), 0xE92D4FF3)
            != hook::Result::Ok)
            perf::Log("item-check hook NOT installed");
        s_buttonSetHooked = s_buttonSetHook.Install(kFnButtonSetPut, reinterpret_cast<void *>(Hooked_ButtonSetPut),
                                                    0xE5D11000) == hook::Result::Ok;
        if (!s_buttonSetHooked)
            perf::Log("button-set hook NOT installed: grapple falls back to the trigger's own bit");
#endif
    }

    std::string CstickStatus()
    {
#ifdef T3D_O3DS
        return Utils::Format("pad reads %lu (Old 3DS build: no C-stick)", s_padCalls);
#endif
        return Utils::Format("cstick game %d,%d conn %d ir %s %d,%d reads %lu aims %lu",
                             s_gameX, s_gameY, s_padConnected, s_irActive ? "on" : "off",
                             s_irX, s_irY, s_padCalls, s_aimCalls);
    }

    bool ShowCstickDebug() { return e_cstickDebug && e_cstickDebug->IsActivated(); }

    void OnFrame()
    {
        // ---- zoom ----
        if (U8(kCameraGuard) & 1)
        {
            const float target = F32(kCamZoomTarget);
            if (s_zoomDirty)
            {
                F32(kCamZoomTarget) = s_zoom2x ? kZoom2x : kZoomNormal;   // the game lerps to it
                s_zoomDirty = false;
            }
            else if (s_zoom2x && target == kZoomNormal && F32(kCamZoom) == kZoomNormal)
            {
                F32(kCamZoomTarget) = kZoom2x;  // a world (re)load reset it to 0.5
            }
        }

        // ---- creative flags (re-applied every frame so world loads can't reset them) ----
        SetFlag(kGodmode,     s_godmode,     s_prevFlags[0]);
        SetFlag(kNoSpawns,    s_noSpawns,    s_prevFlags[1]);
        SetFlag(kFullbright,  s_fullbright,  s_prevFlags[2]);
        SetFlag(kInfBuckets,  s_infBuckets,  s_prevFlags[3]);
        SetFlag(kDebugTools,  s_debugTools,  s_prevFlags[4]);

        // ---- time freeze ----
        if (s_freezeTime)
        {
            if (!s_timeFrozenApplied)
            {
                s_savedTimeRate = F32(kTimeRate);
                s_timeFrozenApplied = true;
            }
            F32(kTimeRate) = 0.0f;
        }
        else if (s_timeFrozenApplied)
        {
            F32(kTimeRate) = s_savedTimeRate > 0.0f ? s_savedTimeRate : 1.0f;
            s_timeFrozenApplied = false;
        }
    }

    bool Zoom2x() { return s_zoom2x; }

    void OnMenuOpening()
    {
        s_resumeWindow = 0;
        s_resumePress = 0;
        // The current page only: the Select press that opens the menu can
        // already have queued the pause (target page 6) by now.
        const u8 page = U8(kUiPage);
        s_menuFromPlay = (U8(kUiGuard) & 1) && (page == kPageMiniInv || page == kPageBuildView)
                      && s_ticksSinceWorldDraw < kWorldDrawGraceTicks;
    }

    void OnMenuClosing()
    {
        if (s_menuFromPlay)
            s_resumeWindow = kResumeTicks;
        s_menuFromPlay = false;
    }

    void BuildMenu(PluginMenu &menu)
    {
        // ---- Zoom ----
        e_zoomHotkey = new MenuEntry("2x zoom hotkey", NoOp,
                                     "Toggles a pixel-perfect 2x camera zoom.\nDefault hotkey: L + R (editable in the hotkey menu).");
        e_zoomHotkey->Hotkeys += Hotkey(Key::L | Key::R, "Toggle 2x zoom");
        e_zoomHotkey->Enable();
        menu.Append(e_zoomHotkey);

        // ---- Controls ----
        MenuFolder *controls = new MenuFolder("Controls");
#ifdef T3D_O3DS
        // Old 3DS build: D-pad cursor + touch (the game's own controls).
        controls->Append(e_padInteract = new MenuEntry("Circle pad: interact only", NoOp,
                                                       "The circle pad's auto-cursor still finds doors, chests\n"
                                                       "and tables, but no longer mines or builds. Aim those with\n"
                                                       "the D-pad cursor or touch. Off by default (it also stops\n"
                                                       "an axe on Y chopping trees)."));
        controls->Append(e_dpadWalk = new MenuEntry("D-pad walking", NoOp,
                                                    "The D-pad moves your character like the circle pad\n"
                                                    "while playing (menus keep the D-pad). Replaces the\n"
                                                    "D-pad cursor. Off by default."));
        menu.Append(controls);
#else
        controls->Append(new MenuEntry("Control preset", nullptr, PresetMenuFunc,
                                       "New 3DS (C-stick aim), Old 3DS (D-pad + touch),\n"
                                       "Stock, or Lefty. Picked automatically on first boot\n"
                                       "from the console model."));
        controls->Append(new MenuEntry("ZL button", nullptr, ZlMenuFunc,
                                       "Switch C-stick style (default: Spring <-> Cursor),\n"
                                       "Grapple, Jump, Use item, or the game's own ZL (build\n"
                                       "view). While playing the game never sees a remapped ZL;\n"
                                       "menus keep their stock ZL."));
        controls->Append(new MenuEntry("ZR button", nullptr, ZrMenuFunc,
                                       "Use item / mine / place at the C-stick aim (default),\n"
                                       "Grapple, Jump, or the game's own ZR (inventory). Menus\n"
                                       "keep their stock ZR."));
        controls->Append(new MenuEntry("C-stick sensitivity", nullptr, SensMenuFunc,
                                       "How far a small push moves the aim. A full push always\n"
                                       "reaches the edge of your reach."));
        controls->Append(new MenuEntry("C-stick smoothing", nullptr, SmoothMenuFunc,
                                       "Steadies a jittery C-stick. Medium is the default."));
        controls->Append(new MenuEntry("C-stick aiming", nullptr, CstickModeMenuFunc,
                                       "The C-stick aims weapons, tools, the grapple and blocks.\n"
                                       "Push a little to aim close, all the way for the edge of\n"
                                       "your reach. Auto-use: a hard push also swings weapons and\n"
                                       "tools (never places blocks or uses consumables)."));
        controls->Append(new MenuEntry("C-stick style", nullptr, CstickStyleMenuFunc,
                                       "How the aim moves. Spring (default, absolute): the push\n"
                                       "sets the aim; let go and the auto-cursor is back.\n"
                                       "Cursor: the stick moves the aim like a mouse and it stays\n"
                                       "put, moving with you; tools mine that spot.\n"
                                       "In play: ZL switches (New 3DS preset)."));
        controls->Append(new MenuEntry("C-stick tool mode", nullptr, ToolMiningMenuFunc,
                                       "What a pickaxe/axe/hammer does on the C-stick (Spring).\n"
                                       "Auto-tunnel (default): push about half way and it digs a\n"
                                       "tunnel your height that way (the game's smart dig).\n"
                                       "Precise: only the aimed tile, never a tunnel."));
        controls->Append(new MenuEntry("Y button", nullptr, YButtonMenuFunc,
                                       "Stock (default): Y uses the item at the game's smart\n"
                                       "cursor while the C-stick is centred. Or: Y uses the item\n"
                                       "at the C-stick aim, like ZR."));
        controls->Append(new MenuEntry("Cursor speed", nullptr, CursorSpeedMenuFunc,
                                       "How fast the Cursor-style aim moves at a full push\n"
                                       "(Slow 135, Normal 195, Fast 300 px/s). Small pushes creep."));
        controls->Append(new MenuEntry("Cursor auto-hide", nullptr, CursorHideMenuFunc,
                                       "Cursor style: the crosshair hides while you walk with the\n"
                                       "C-stick idle (default), also after 3 s idle, or never.\n"
                                       "It comes back at the same spot on the next push."));
        controls->Append(e_padInteract = new MenuEntry("Circle pad: interact only", NoOp,
                                                       "The circle pad's auto-cursor still finds doors, chests\n"
                                                       "and tables, but no longer mines or builds. Aim those with\n"
                                                       "the C-stick, the D-pad cursor or touch. Off by default\n"
                                                       "(it also stops an axe on Y chopping trees)."));
        controls->Append(e_dpadWalk = new MenuEntry("D-pad walking", NoOp,
                                                    "The D-pad moves your character like the circle pad\n"
                                                    "while playing (menus keep the D-pad). Replaces the\n"
                                                    "D-pad cursor. Off by default."));
        controls->Append(e_cstickDebug = new MenuEntry("C-stick debug (bottom screen)", NoOp,
                                                       "Raw C-stick values from the game and from ir:rst."));
        menu.Append(controls);
#endif

        // ---- Display ----
        menu.Append(new MenuEntry("Colour booster (fake OLED)", nullptr, ColorModeMenuFunc,
                                  "Deep blacks and punchier contrast on both screens,\n"
                                  "applied by the display hardware (no FPS cost).\n"
                                  "Replaces any Rosalina screen filter while on."));

        // ---- Cheats ----
        MenuFolder *cheats = new MenuFolder("Cheats");
        e_consoleHotkey = new MenuEntry("Cheat console hotkey", NoOp,
                                        "Opens the game's hidden developer cheat keyboard.\nDefault hotkey: L + R + Y.");
        e_consoleHotkey->Hotkeys += Hotkey(Key::L | Key::R | Key::Y, "Open cheat console");
        e_consoleHotkey->Enable();
        cheats->Append(e_consoleHotkey);
        cheats->Append(new MenuEntry("Open cheat console now", nullptr, CheatConsoleMenuFunc,
                                     "Type a command such as: item torch, godmode, noon, meteor."));
        menu.Append(cheats);

        // ---- Creative mode ----
        MenuFolder *creative = new MenuFolder("Creative mode");
        creative->Append(e_godmode     = new MenuEntry("Godmode", NoOp, "No damage."));
        creative->Append(e_noSpawns    = new MenuEntry("No enemy spawns", NoOp, "Stops enemy spawning."));
        creative->Append(e_fullbright  = new MenuEntry("Fullbright", NoOp, "Lighting off: everything is lit."));
        creative->Append(e_infItems    = new MenuEntry("Infinite blocks, potions, ammo & wire", NoOp,
                                                       "Placing, drinking, firing and wiring no longer use up items."));
        creative->Append(e_infBuckets  = new MenuEntry("Infinite buckets", NoOp, "Buckets never empty."));
        creative->Append(e_noFall      = new MenuEntry("No fall damage", NoOp, "Fall from any height."));
        creative->Append(e_instantMine = new MenuEntry("Instant mining", NoOp, "Any pickaxe/axe breaks any tile in one hit."));
        creative->Append(e_reach       = new MenuEntry("Long reach", NoOp, "Use and place up to about 32 tiles away."));
        creative->Append(e_flight      = new MenuEntry("Flight (Circle Pad)", NoOp,
                                                       "Experimental. Circle Pad flies in any direction; let go to hover."));
        creative->Append(e_freezeTime  = new MenuEntry("Freeze time", NoOp, "Stops the day/night clock."));
        creative->Append(new MenuEntry("Set time to noon", nullptr, NoonMenuFunc, "Jumps to midday."));
        creative->Append(e_debugTools  = new MenuEntry("Terrain tools (debug mode)", NoOp,
                                                       "Experimental. The game's debug cursor tools; choose one with the cheat "
                                                       "console (tile/erase/water/lava/mine/size). May warp you to the cursor."));
        menu.Append(creative);

        menu += PollMenu;
    }
}
