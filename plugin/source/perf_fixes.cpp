#include "perf_fixes.hpp"

#include "hooks.hpp"
#include "memory.hpp"
#include "stereo_present.hpp"

#include <cstdio>

// Cave backdrop culling. Every fact below is read from the v5.6.0 code;
// notes/perf.md has the evidence for each one.
//
// Tile (14 bytes, column-major: tile(x,y) = base + (x*stride + y)*14)
//   +1  u8   bit0 active, bit1 inactive (actuated), bit2 half brick,
//            bits3-5 slope
//   +3  u8   liquid amount
//   +6  u16  bits0-8 type, bits9-15 wall
//   +8  s16  frameX         +10 s16 frameY
// Tile property table 0x851B14, 16 bytes per type:
//   word0 bit0 tileSolid, bit1 (excluded, solid-top style), bit11 blockLight
//   +6 u8 draw width, +7 u8 draw height (source rect size in the tile pass)
//
// Solid-tile pass 0x51A71C draws a tile T at (x*16, y*16) - screenPos with
// source (frameX, frameY + anim, w, h), alpha 255, iff:
//   x in [Main+0xE8, Main+0xEA), y in [Main+0xEC, Main+0xEE), active,
//   tileSolid, and lit R+G+B > 11. Inactive tiles are dimmed, half bricks /
//   slopes / half-brick neighbours take other draw paths: all excluded here.

namespace t3d::perf_fixes
{
    namespace
    {
        // ---- game addresses (v5.6.0) ----
        constexpr u32 kFnVisible     = 0x00523CF4; // backdrop cell visibility (replaced)
        constexpr u32 kVisibleInsn0  = 0xE92D4010;
        constexpr u32 kVisibleInsn1  = 0xE3520001;
        constexpr u32 kFnDraw        = 0x001DBC64; // SpriteBatch draw(tex, pos*, src*, color, 0)
        constexpr u32 kCave          = 0x001DEE7C; // unreferenced cell-draw variant, first 8 bytes
        constexpr u32 kCaveInsn0     = 0xE92D4FFF;
        constexpr u32 kCaveInsn1     = 0xE1A05003;
        // LR values of the two live calls to kFnVisible (r1 = Main there)
        constexpr u32 kRetVisRow     = 0x001DCDB4; // in 0x1DCB98 (edge rows)
        constexpr u32 kRetVisCell    = 0x001DEC60; // in 0x1DEBCC (main cells)

        struct Site { u32 addr, original, cond; };
        // Every draw call in the two live backdrop cell functions.
        constexpr Site kDrawSites[] = {
            { 0x001DCE88, 0xEBFFFB75, 0xE }, // 0x1DCB98 cell
            { 0x001DCFDC, 0xEBFFFB20, 0xE }, // 0x1DCB98 overlay (biome blend)
            { 0x001DED0C, 0x1BFFF3D4, 0x1 }, // 0x1DEBCC cell      (blne)
            { 0x001DEE34, 0x1BFFF38A, 0x1 }, // 0x1DEBCC overlay   (blne)
        };

        constexpr u32 kTiles         = 0x007C4B20; // u32 base; +0x10 s32 column stride
        constexpr u32 kMaxTilesX     = 0x006333A6; // s16
        constexpr u32 kMaxTilesY     = 0x006333A8; // s16
        constexpr u32 kLightMin      = 0x005C2B8C; // s32 (12): cell drawn if R+G+B > this
        constexpr u32 kTileProps     = 0x00851B14;
        constexpr u32 kTileAnim      = 0x00854780; // per-type animation object ptr (0 = none)

        // Main fields
        constexpr u32 kScreenX = 0x110, kScreenY = 0x114;
        constexpr u32 kTileX0 = 0xE8, kTileX1 = 0xEA, kTileY0 = 0xEC, kTileY1 = 0xEE; // s16
        constexpr u32 kLightOrgX = 0x1A84, kLightOrgY = 0x1A88;  // s32
        constexpr u32 kLightScale = 0x1A28;                       // float
        constexpr u32 kLightBase = 0x1A38, kLightStride = 0x1A48; // 3 x s32 per tile

        constexpr u32 kTileSize = 14;
        constexpr u8  kActive = 0x01, kInactive = 0x02, kHalfBrick = 0x04, kSlope = 0x38;
        constexpr u32 kPropSolid = 0x1, kPropSolidTop = 0x2;

        volatile bool s_cull = true;
        bool          s_installed = false;
        u32           s_main = 0;   // set by the visibility replacement each cell

        template <typename T> inline T Rd(u32 a) { return *reinterpret_cast<const volatile T *>(a); }

        // Natural / brick block types (PC 1.2 ids; the 3DS uses the same ids:
        // 5 tree, 19 platform, 72 mushroom, 80 cactus, 128 mannequin are
        // special-cased by those numbers in the tile passes). Glass (54),
        // platforms, doors, traps, boulders, ice, honey, spikes and every
        // furniture / frame-important tile are deliberately absent.
        constexpr u16 kOpaqueTypes[] = {
            0, 1, 2, 6, 7, 8, 9, 22, 23, 25, 30, 37, 38, 39, 40, 41, 43, 44, 45, 46, 47,
            53, 56, 57, 58, 59, 60, 63, 64, 65, 66, 67, 68, 70, 75, 76,
            107, 108, 109, 111, 112, 116, 117, 118, 119, 120, 121, 122, 123, 140,
            147, 166, 167, 168, 169, 199, 203, 204, 211, 221, 222, 223, 224, 225, 226, 234,
        };
        u32 s_opaque[512 / 32];

        void BuildTypeMask()
        {
            for (u16 t : kOpaqueTypes)
                s_opaque[t >> 5] |= 1u << (t & 31);
        }

        inline bool TypeAllowed(u32 type) { return (s_opaque[type >> 5] >> (type & 31)) & 1; }

        // Exact port of 0x523CF4 (plus recording Main).
        // r0 unused, r1 Main (live callers), r2/r3 tile x/y, [sp] u8 rgb[3].
        int Visible(u32, u32 main, int x, int y, const u8 *rgb)
        {
            const u32 lr = reinterpret_cast<u32>(__builtin_return_address(0));
            if (lr == kRetVisRow || lr == kRetVisCell)
                s_main = main;

            if (x < 1 || x >= Rd<s16>(kMaxTilesX) - 1) return 0;
            if (y < 1 || Rd<s16>(kMaxTilesY) - 1 <= y) return 0;
            const s32 sum = static_cast<s32>(rgb[0]) + rgb[1] + rgb[2];
            if (sum <= Rd<s32>(kLightMin)) return 0;

            const u32 t = Rd<u32>(kTiles) + static_cast<u32>(x * Rd<s32>(kTiles + 0x10) + y) * kTileSize;
            if ((Rd<u16>(t + 6) & 0xFE00) == 0) return 1;
            const s32 col = static_cast<s32>(Rd<s16>(kMaxTilesY)) * kTileSize; // 0x3E58E0 / 0x3E58FC
            if ((Rd<u16>(t - col + 6) & 0xFE00) == 0) return 1;
            if ((Rd<u16>(t + col + 6) & 0xFE00) == 0) return 1;
            return 0;
        }

        // True only if the solid-tile pass will cover tile (x,y) with a fully
        // opaque 16x16 interior block frame this frame.
        bool TileCovers(u32 main, int x, int y)
        {
            if (x < Rd<s16>(main + kTileX0) || x >= Rd<s16>(main + kTileX1)) return false;
            if (y < Rd<s16>(main + kTileY0) || y >= Rd<s16>(main + kTileY1)) return false;
            if (x < 1 || x > Rd<s16>(kMaxTilesX) - 2 || y < 0 || y >= Rd<s16>(kMaxTilesY)) return false;

            const u32 base = Rd<u32>(kTiles);
            const u32 t = base + static_cast<u32>(x * Rd<s32>(kTiles + 0x10) + y) * kTileSize;
            const u8 f = Rd<u8>(t + 1);
            if ((f & (kActive | kInactive | kHalfBrick | kSlope)) != kActive) return false;

            const u32 type = Rd<u16>(t + 6) & 0x1FF;
            if (!TypeAllowed(type)) return false;
            const u32 prop = kTileProps + type * 16;
            const u32 w0 = Rd<u32>(prop);
            if (!(w0 & kPropSolid) || (w0 & kPropSolidTop)) return false;
            if (Rd<u8>(prop + 6) != 16 || Rd<u8>(prop + 7) != 16) return false;
            if (Rd<u32>(kTileAnim + type * 4) != 0) return false;   // animated frames

            // Interior frame only: all four sides merged, no edge outline.
            const s16 fx = Rd<s16>(t + 8), fy = Rd<s16>(t + 10);
            if (fy != 18 || (fx != 18 && fx != 36 && fx != 54)) return false;

            // A half-brick left/right neighbour makes 0x51A71C draw this tile
            // in pieces from other frames.
            const s32 col = static_cast<s32>(Rd<s16>(kMaxTilesY)) * kTileSize;
            if ((Rd<u8>(t - col + 1) & kHalfBrick) || (Rd<u8>(t + col + 1) & kHalfBrick)) return false;

            // Lit enough to be drawn at all (0x51A71C: R+G+B > 11), same maths
            // as 0x51A8E8..0x51A9F8, with a margin for float rounding.
            const u32 lbase = Rd<u32>(main + kLightBase);
            if (!lbase) return false;
            const s32 lx = x - Rd<s32>(main + kLightOrgX), ly = y - Rd<s32>(main + kLightOrgY);
            if (lx < 0 || ly < 0) return false;
            const u32 e = lbase + static_cast<u32>(lx * Rd<s32>(main + kLightStride) + ly) * 12;
            const float scale = Rd<float>(main + kLightScale);
            s32 sum = 0;
            for (int i = 0; i < 3; i++)
            {
                s32 c = static_cast<s32>(static_cast<float>(Rd<s32>(e + 4 * i)) * scale);
                if (c > 0x400) c = 0x400;
                if (c < 0) c = 0;
                sum += static_cast<s32>(static_cast<float>(c) * (1.0f / 1024.0f) * 255.0f);
            }
            return sum >= 16;
        }

        inline int FloorDiv16(s32 v) { return v >> 4; }   // arithmetic shift = floor

        bool CellHidden(const float *pos, const s32 *src)
        {
            const u32 main = s_main;
            if (!main) return false;
            const s32 w = src[2], h = src[3];
            if (w <= 0 || h <= 0 || w > 32 || h > 32) return false;
            const s32 sx = static_cast<s32>(pos[0]), sy = static_cast<s32>(pos[1]);
            if (static_cast<float>(sx) != pos[0] || static_cast<float>(sy) != pos[1]) return false;

            // Stereo: the backdrop and tile layers are shifted against each
            // other by up to 2x the disparity (screen px >= world px at every
            // zoom), so a cell must also be covered that far to each side.
            s32 margin = 0;
            namespace sp = t3d::stereo::present;
            if (sp::Enabled())
                margin = 2 * sp::MaxDisparity() + 2;

            const s32 wx = sx + Rd<s32>(main + kScreenX), wy = sy + Rd<s32>(main + kScreenY);
            const int x0 = FloorDiv16(wx - margin), x1 = FloorDiv16(wx + w - 1 + margin);
            const int y0 = FloorDiv16(wy), y1 = FloorDiv16(wy + h - 1);
            for (int x = x0; x <= x1; x++)
                for (int y = y0; y <= y1; y++)
                    if (!TileCovers(main, x, y))
                        return false;
            return true;
        }

        using DrawFn = void (*)(s32, const float *, const s32 *, u32, u32);

        // ---------------------------------------------------------------
        // Viewport cull. 0x1DBC64 -> 0x1DB3DC -> 0x1DB2A0 -> 0x2204A4 ->
        // 0x1D975C builds the quad on the CPU (notes/perf.md, "Backdrop cost"):
        //   p  = pos + batch.off34                      (+0x34/+0x38)
        //   if (!batch.noZoom /*+0x3C*/) p = (p - c) * zoom + c
        //   p += batch.translate                        (+0x2C/+0x30)
        //   quad = [p, p + src.wh * zoom]               (origin 0 on this path)
        // c = screen centre = *(batch+0x70) -> [+0x28] idx -> +0x18 + idx*8.
        // The screen is [0, 2c]. A cell is skipped only when its quad, grown
        // to twice its size and padded by the stereo margin, is off screen.
        constexpr u32 kBatchHolder = 0x0086D278; // g_batch: [0] = SpriteBatch*
        constexpr u32 kSbTranslate = 0x2C, kSbOff = 0x34, kSbNoZoom = 0x3C, kSbZoom = 0x4C;
        constexpr u32 kSbScreen = 0x70;

        volatile bool s_viewport = true;

        bool OffScreen(const float *pos, const s32 *src)
        {
            const u32 b = Rd<u32>(kBatchHolder);
            if (!b) return false;
            const u32 scr = Rd<u32>(b + kSbScreen);
            if (!scr) return false;
            const u32 cen = scr + 0x18 + Rd<u32>(scr + 0x28) * 8;
            const float cx = Rd<float>(cen), cy = Rd<float>(cen + 4);
            if (!(cx >= 100.0f && cx <= 400.0f && cy >= 60.0f && cy <= 240.0f))
                return false;                       // not a screen we know
            const float z = Rd<float>(b + kSbZoom);
            if (!(z >= 0.1f && z <= 4.0f))
                return false;

            float x = pos[0] + Rd<float>(b + kSbOff), y = pos[1] + Rd<float>(b + kSbOff + 4);
            if (!Rd<u8>(b + kSbNoZoom))
            {
                x = (x - cx) * z + cx;
                y = (y - cy) * z + cy;
            }
            x += Rd<float>(b + kSbTranslate);
            y += Rd<float>(b + kSbTranslate + 4);
            const float w = static_cast<float>(src[2]) * z, h = static_cast<float>(src[3]) * z;

            namespace sp = t3d::stereo::present;
            const float mx = 8.0f + (sp::Enabled() ? static_cast<float>(2 * sp::MaxDisparity() + 2) : 0.0f);
            const float my = 8.0f;
            return x + 2.0f * w < -mx || x - w > 2.0f * cx + mx ||
                   y + 2.0f * h < -my || y - h > 2.0f * cy + my;
        }

        // ---------------------------------------------------------------
        // Run merge. The pass submits cells in a fixed order: the main grid
        // (0x1DEBCC) walks a 128x96 block column by column, 6 cells down;
        // the edge rows (0x1DCB98) walk 8 cells across. Consecutive cells are
        // therefore neighbours on screen AND in the texture (src steps by the
        // cell size exactly as pos does). A run of such cells with the same
        // texture and colour is drawn as one sprite with the union rectangle:
        // same pixels, one submission instead of up to 6 or 8.
        //
        // Atlas textures are split into chunks; 0x1DB4C0 picks the chunk from
        // src / chunkSize and clips src to it, so a merged rect must stay in
        // one chunk. Chunk size as 0x1DB2A0 computes it:
        //   obj = *(0x8D5024+0x7C)[tex] (0 = plain texture, no chunks)
        //   cw = ((s16)obj[0] + u8 obj[0x12]) * 2, ch = ((s16)obj[2] + ...) * 2
        constexpr u32 kTexMgr = 0x008D5024;   // 0x5A3D38() singleton

        volatile bool s_merge = true, s_fastLight = true, s_passHooked = false;
        constexpr int kFastTol = 12;          // per channel, of 255

        struct Pending
        {
            bool  live = false;
            s32   tex = 0;
            u32   a4 = 0, color0 = 0;
            float pos[2] = {};
            s32   src[4] = {};
            s32   cw = 0, ch = 0;            // chunk size, 0 = none
            int   dir = 0;                   // 0 single, 1 down, 2 across
            int   n = 0;
            u32   sumR = 0, sumG = 0, sumB = 0;
            s32   lastSrcX = 0, lastSrcY = 0; // last cell's src origin
            float lastX = 0, lastY = 0;
        };
        Pending s_pend;

        // Stats (game thread writes, plugin thread reads; approximate)
        volatile u32 s_stPasses = 0, s_stIn = 0, s_stCover = 0, s_stView = 0, s_stMerged = 0, s_stOut = 0;

        void ChunkSize(s32 tex, s32 &cw, s32 &ch)
        {
            cw = ch = 0;
            const u32 begin = Rd<u32>(kTexMgr + 0x7C), end = Rd<u32>(kTexMgr + 0x80);
            if (tex < 0 || static_cast<u32>(tex) >= (end - begin) / 4)
                return;
            const u32 obj = Rd<u32>(begin + static_cast<u32>(tex) * 4);
            if (!obj)
                return;
            const s32 pad = Rd<u8>(obj + 0x12);
            cw = (Rd<s16>(obj) + pad) * 2;
            ch = (Rd<s16>(obj + 2) + pad) * 2;
            if (cw <= 0 || ch <= 0)
                cw = ch = -1;                 // unknown: never merge
        }

        inline bool SameChunk(s32 a, s32 b, s32 size)
        {
            return size == 0 || (size > 0 && a >= 0 && b >= 0 && a / size == b / size);
        }

        inline bool ColorClose(u32 a, u32 b, int tol)
        {
            if ((a >> 24) != (b >> 24)) return false;
            for (int sh = 0; sh < 24; sh += 8)
            {
                const int d = static_cast<int>((a >> sh) & 0xFF) - static_cast<int>((b >> sh) & 0xFF);
                if (d > tol || d < -tol) return false;
            }
            return true;
        }

        void Flush()
        {
            Pending &p = s_pend;
            if (!p.live)
                return;
            p.live = false;
            u32 color = p.color0;
            if (p.n > 1 && s_fastLight)
            {
                const u32 r = (p.sumR + p.n / 2) / p.n, g = (p.sumG + p.n / 2) / p.n, bl = (p.sumB + p.n / 2) / p.n;
                color = (p.color0 & 0xFF000000u) | (bl << 16) | (g << 8) | r;
            }
            s_stOut = s_stOut + 1;
            reinterpret_cast<DrawFn>(kFnDraw)(p.tex, p.pos, p.src, color, p.a4);
        }

        void Start(s32 tex, const float *pos, const s32 *src, u32 color, u32 a4)
        {
            Pending &p = s_pend;
            p.live = true;
            p.tex = tex; p.a4 = a4; p.color0 = color;
            p.pos[0] = pos[0]; p.pos[1] = pos[1];
            for (int i = 0; i < 4; i++) p.src[i] = src[i];
            ChunkSize(tex, p.cw, p.ch);
            p.dir = 0; p.n = 1;
            p.sumR = color & 0xFF; p.sumG = (color >> 8) & 0xFF; p.sumB = (color >> 16) & 0xFF;
            p.lastSrcX = src[0]; p.lastSrcY = src[1];
            p.lastX = pos[0]; p.lastY = pos[1];
        }

        // Can (pos, src) extend the pending run? Cells are 16x16 here; the
        // step must be the cell size in BOTH pos and src (dest size == src
        // size on this path), so a scaled or gapped draw never merges.
        bool TryExtend(s32 tex, const float *pos, const s32 *src, u32 color, u32 a4)
        {
            Pending &p = s_pend;
            if (!p.live || tex != p.tex || a4 != p.a4 || p.cw < 0)
                return false;
            if (!ColorClose(color, p.color0, s_fastLight ? kFastTol : 0))
                return false;
            const s32 w = src[2], h = src[3];
            if (w != 16 || h != 16) return false;

            const bool down = p.dir != 2 && src[0] == p.lastSrcX && src[1] == p.lastSrcY + 16 &&
                              pos[0] == p.lastX && pos[1] == p.lastY + 16.0f && p.src[2] == 16;
            const bool across = !down && p.dir != 1 && src[1] == p.lastSrcY && src[0] == p.lastSrcX + 16 &&
                                pos[1] == p.lastY && pos[0] == p.lastX + 16.0f && p.src[3] == 16;
            if (!down && !across)
                return false;
            // Whole merged rect inside one atlas chunk
            if (!SameChunk(p.src[0], src[0] + w - 1, p.cw) || !SameChunk(p.src[1], src[1] + h - 1, p.ch))
                return false;
            if (!SameChunk(p.src[0], p.src[0] + p.src[2] - 1, p.cw) || !SameChunk(p.src[1], p.src[1] + p.src[3] - 1, p.ch))
                return false;

            if (down) { p.dir = 1; p.src[3] += 16; }
            else      { p.dir = 2; p.src[2] += 16; }
            p.n++;
            p.sumR += color & 0xFF; p.sumG += (color >> 8) & 0xFF; p.sumB += (color >> 16) & 0xFF;
            p.lastSrcX = src[0]; p.lastSrcY = src[1];
            p.lastX = pos[0]; p.lastY = pos[1];
            s_stMerged = s_stMerged + 1;
            return true;
        }

        // Reached from the 4 redirected `bl`s through the cave jump; same
        // arguments as 0x1DBC64 (5th on the stack).
        void BackdropDraw(s32 tex, const float *pos, const s32 *src, u32 color, u32 a4)
        {
            s_stIn = s_stIn + 1;
            if (s_cull && CellHidden(pos, src))
            {
                s_stCover = s_stCover + 1;
                return;
            }
            if (s_viewport && OffScreen(pos, src))
            {
                s_stView = s_stView + 1;
                return;
            }
            if (!(s_merge && s_passHooked))
            {
                s_stOut = s_stOut + 1;
                reinterpret_cast<DrawFn>(kFnDraw)(tex, pos, src, color, a4);
                return;
            }
            if (TryExtend(tex, pos, src, color, a4))
                return;
            Flush();
            Start(tex, pos, src, color, a4);
        }

        // ---------------------------------------------------------------
        // No catch-up: GameState::Update (0x50F554) runs Main tick while
        // acc >= 1/60, acc -= 1/60; the loop's back edge is 0x50FAA4
        // `bge 0x50F82C`. A NOP there ends the loop after one tick. The
        // leftover in acc (+0x28) is clamped by the game itself (acc > 1.0 ->
        // 1/60 at 0x50F7F8), so it cannot grow without bound.
        constexpr u32 kCatchUpBranch = 0x0050FAA4;
        constexpr u32 kCatchUpInsn   = 0xAAFFFF60;
        volatile bool s_noCatchUp = false;
    }

    void Install()
    {
        if (s_installed)
            return;
        BuildTypeMask();

        u32 w0 = 0, w1 = 0, v0 = 0, v1 = 0;
        mem::Read32(kCave, w0);
        mem::Read32(kCave + 4, w1);
        mem::Read32(kFnVisible, v0);
        mem::Read32(kFnVisible + 4, v1);
        for (const Site &s : kDrawSites)
        {
            u32 cur = 0;
            if (!mem::Read32(s.addr, cur) || cur != s.original)
                return;
        }
        if (w0 != kCaveInsn0 || w1 != kCaveInsn1 || v0 != kVisibleInsn0 || v1 != kVisibleInsn1)
            return;

        // Replace the visibility check first (behaviour-identical, records Main),
        // then the cave, then point the draw calls at it.
        if (hook::ReplaceFunction(kFnVisible, reinterpret_cast<void *>(Visible), kVisibleInsn0) != hook::Result::Ok)
            return;
        if (!mem::PatchAbsJump(kCave, reinterpret_cast<u32>(BackdropDraw)))
            return;
        bool all = true;
        for (const Site &s : kDrawSites)
            all &= mem::Replace32(s.addr, s.original, mem::EncodeBranch(s.addr, kCave, true, s.cond));
        s_installed = all;
    }

    void SetBackdropCull(bool on) { s_cull = on; }
    bool BackdropCull()           { return s_cull; }
    bool BackdropCullInstalled()  { return s_installed; }
    void SetViewportCull(bool on) { s_viewport = on; }
    void SetMergeRuns(bool on)    { s_merge = on; }
    void SetFastLighting(bool on) { s_fastLight = on; }
    void SetPassHooked(bool on)   { s_passHooked = on; }

    void BackdropPassBegin()
    {
        s_pend.live = false;   // never carry a run across passes
    }

    void BackdropPassEnd()
    {
        Flush();
        s_stPasses = s_stPasses + 1;
    }

    std::string TakeStats()
    {
        const u32 n = s_stPasses;
        char buf[96];
        if (!n)
            return "bg -";
        snprintf(buf, sizeof(buf), "bg in %lu cov %lu vp %lu mrg %lu out %lu",
                 s_stIn / n, s_stCover / n, s_stView / n, s_stMerged / n, s_stOut / n);
        s_stPasses = 0; s_stIn = 0; s_stCover = 0; s_stView = 0; s_stMerged = 0; s_stOut = 0;
        return buf;
    }

    void SetNoCatchUp(bool on)
    {
        if (on == s_noCatchUp)
            return;
        const bool ok = on ? mem::Replace32(kCatchUpBranch, kCatchUpInsn, mem::kInsnNop)
                           : mem::Replace32(kCatchUpBranch, mem::kInsnNop, kCatchUpInsn);
        if (ok)
            s_noCatchUp = on;
    }

    bool NoCatchUp() { return s_noCatchUp; }
#ifdef T3D_O3DS
    u32  DrawMain()  { return s_main; }
    void ClearDrawMain() { s_main = 0; }
#endif
}
