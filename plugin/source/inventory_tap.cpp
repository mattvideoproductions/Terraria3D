#include "inventory_tap.hpp"

#include <CTRPluginFramework.hpp>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "hooks.hpp"
#include "memory.hpp"
#include "perf.hpp"

// Tap to pick, tap to place (Vita style) for DSInventoryItemGrid.
// notes/inventory.md has the evidence for every address and offset below.
//
// Why v0.5.2 did nothing on hardware (static analysis, notes/inventory.md
// "Touch timing"): a Widget fires "tap" (event 3) only when released before
// +0x94 = 0.25 s, and fires "hold" (event 4) once a press lasts longer than
// +0x90 = 0.25 s, after which tap can no longer fire. On a non-empty slot the
// hold starts the game's touch drag (move state, mode 0), so a normal-speed
// tap became "drag, then drop on the same slot": the lifted-icon "hover
// wiggle" and nothing else. v0.5.2 only acted on a tap followed by a release
// in the normal state, which a drag never produces.
//
// v0.5.3: every way a press can start and end on one slot is a tap:
//   quick tap      ListView child tap 0x48C8C8 (event 3)
//   slow tap       grid release-on-slot 0x253320, touch began on the slot and
//                  ends inside it, state normal or carry (no drag running)
//   held tap       grid "dropped" 0x253400 with drop target == drag source
// The hooks only record; the action runs in the grid's own Update (0x254180),
// after the touch pass, the drag helper's cleanup and any signal emission
// have finished, with the slot checked against the grid's slot list first.
//
// v0.5.4 (why v0.5.3 picked but never placed on hardware): the game's carry
// (0x252A90, mode 1) disables touch on the whole slot list (ListView vt+0x14
// -> every widget +0x88 = 0), so the second tap never reached any hook. After
// a tap pick-up the list's touch is enabled again (vt+0x10, what the game's
// own return to normal does). Chest <-> inventory: the tabs are separate
// grids, so the carry's grid is hidden and its state reset; the picked Item*
// is kept as a "pending" carry and the next slot tap in the new tab places
// it with the game's own rules (0x1866FC, 0x3C76F4 merge/move, 0x3C766C swap).
//
// DSInventoryItemGrid (ctor 0x254748, vtable 0x5D7E9C, 0x50 bytes):
//   +0x28 ItemWidgetDragDropHelper*   (+0x38 u8 enabled, +0x3C source, +0x40 target)
//   +0x30 ListView* (the slot list; children are ItemWidgets, +0x160 = Item*)
//   +0x34/+0x38 vector<ItemWidget*> begin/end
//   +0x40 normal state   (vtable 0x5D83E0)
//   +0x44 move state     (vtable 0x5D7E48; +0x08 source ItemWidget,
//                         +0x10 u8 mode: 0 = touch drag, 1 = carry (A button))
//   +0x48 current state
// Actions (the game's own paths only):
//   0x253380(grid, w)  "A on slot": normal -> 0x276B0C pick up into carry (refuses
//                      empty slots and grids whose helper is disabled); carry ->
//                      0x2522AC place / merge / swap with the slot rules, then normal.
//   0x253208(grid, w)  back to normal state (carry cancelled, item never moved).
// Page close: the grid's hide (0x252B1C) exits the state through the game, so a
// carried item is simply left where it always was.

namespace t3d::inventory_tap
{
    namespace
    {
        // ---- game addresses (v5.6.0) ----
        constexpr u32 kFnLvChildTap    = 0x0048C8C8; // ListView: child widget tapped (child event 3)
        constexpr u32 kLvChildTapInsn  = 0xE92D4070; //   push {r4, r5, r6, lr}
        constexpr u32 kFnGridRelease   = 0x00253320; // grid: ListView+0x40 (released on child) -> state+0x20
        constexpr u32 kGridReleaseInsn = 0xE5900048; //   ldr r0, [r0, #0x48]
        constexpr u32 kFnGridDropped   = 0x00253400; // grid: helper+0x28 (dropped) -> state+0x40
        constexpr u32 kGridDroppedInsn = 0xE92D4070; //   push {r4, r5, r6, lr}
        constexpr u32 kFnGridUpdate    = 0x00254180; // grid vtable +0x08 Update(grid, s0 = dt)
        constexpr u32 kGridUpdateInsn  = 0xE92D4010; //   push {r4, lr}

        constexpr u32 kFnGridActivate  = 0x00253380; // grid: ListView+0x158 ("A" on slot) -> state+0x34
        constexpr u32 kFnGridToNormal  = 0x00253208; // grid: back to normal state, select widget
        constexpr u32 kFnInput         = 0x005A3BB4; // input singleton getter
        constexpr u32 kFnPressed       = 0x00524028; // (input, button index) -> pressed this frame
        constexpr u32 kFnTouch         = 0x001EDA0C; // (input) -> current touch or null
        constexpr u32 kFnTouchPos      = 0x001CBA60; // (float out[2], touch)
        constexpr int kButtonB         = 2;          // notes/aiming.md: A=1, B=2

        constexpr u32 kGridHelper      = 0x28;
        constexpr u32 kGridLv          = 0x30;
        constexpr u32 kGridSlotsBegin  = 0x34;
        constexpr u32 kGridSlotsEnd    = 0x38;
        constexpr u32 kGridNormal      = 0x40;
        constexpr u32 kGridMove        = 0x44;
        constexpr u32 kGridState       = 0x48;
        constexpr u32 kMoveSrc         = 0x08;
        constexpr u32 kMoveMode        = 0x10;
        constexpr u8  kModeCarry       = 1;
        constexpr u8  kModeDrag        = 0;
        constexpr u32 kHelperEnabled   = 0x38;
        constexpr u32 kHelperSource    = 0x3C;
        constexpr u32 kHelperTarget    = 0x40;
        constexpr u32 kWidgetVtRect    = 0x58;       // Widget vtable: GetRect(Rect *out, Widget *)
        constexpr u32 kWidgetPressedHere = 0x8B;     // u8: the current touch began on this widget
        constexpr u32 kSlotItem        = 0x160;
        constexpr u32 kSlotType        = 0x18C;      // u8 slot kind for 0x1866FC (0 = any item)
        constexpr u32 kGridSource      = 0x2C;       // data source (chest / inventory tab swaps it, 0x2527DC)
        constexpr u32 kWidgetVtEnable  = 0x10;       // Widget vtable: enable touch, recursive (+0x88 = 1; 0x1E18C8 for ListView)
        constexpr u32 kFnSlotAccepts   = 0x001866FC; // (Item*, slot kind) -> can that slot hold the item
        constexpr u32 kFnItemMerge     = 0x003C76F4; // (src, dst): merge / move into empty -> 0/1, different -> 2
        constexpr u32 kFnItemSwap      = 0x003C766C; // (a, b): swap 0x84 bytes
        constexpr u64 kTicksPerSec     = 268111856ull;        // SYSCLOCK_ARM11
        constexpr u64 kHiddenTicks     = kTicksPerSec / 10;   // carry grid silent this long (another grid updating) = tab switched away
        constexpr u64 kPendingTicks    = kTicksPerSec / 2;    // no grid updating this long = inventory closed: drop any carry state
        constexpr u32 kHiddenUpdates   = 4;                    // ... and at least this many updates of another grid
        constexpr u32 kItemStack       = 0x28;
        constexpr u32 kItemType        = 0x52;
        constexpr int kMaxSlots        = 256;
        constexpr u8  kRecordMaxAge    = 8;          // grid updates before an unmatched record is dropped

        using FnGridWidget = void (*)(void *grid, void *widget);
        using FnInput      = void *(*)();
        using FnPressed    = bool (*)(void *input, int button);
        using FnTouch      = void *(*)(void *input);
        using FnTouchPos   = void (*)(float *out, void *touch);
        using FnRect       = void (*)(float *out, void *widget);

        hook::FunctionHook s_tapHook, s_releaseHook, s_droppedHook, s_updateHook, s_touchReadHook;

        // ---- emulator test input (inert on hardware) ----
        // Azahar's window can't take synthetic mouse input while the harness
        // stays focus-free, and its RPC can't write plugin memory, so the
        // harness (tools/azahar_touch.py) writes a touch into the zero padding
        // after the game's .text (0x5C0BBC..0x5C0FFF, same mapped page, never
        // written by the game). On a console nothing writes there, so the
        // magic never appears and the hook only calls the game.
        constexpr u32 kFnTouchRead     = 0x002B4A78; // (TouchReader*, u8 status[5]): x u16, y u16, touched u8; sole caller 0x3FA7E0
        constexpr u32 kTouchReadInsn   = 0xE92D4010; //   push {r4, lr}
        constexpr u32 kTestTouchMagic  = 0x54335454; // "TT3T"
        constexpr u32 kTestTouchBox    = 0x005C0F00; // [0] magic = touching, [1] x | y << 16 (bottom screen px)

        void Note(const char *fmt, ...);

        bool Hooked_TouchRead(void *reader, u8 *status)
        {
            bool ok = hook::FunctionHook::CallOriginal<bool>(reader, status);
            const volatile u32 *box = reinterpret_cast<const volatile u32 *>(kTestTouchBox);
            if (box[0] == kTestTouchMagic && status)
            {
                const u32 xy = box[1];
                *reinterpret_cast<u16 *>(status + 0) = static_cast<u16>(xy & 0xFFFF);
                *reinterpret_cast<u16 *>(status + 2) = static_cast<u16>(xy >> 16);
                status[4] = 1;
                ok = true;
            }
            return ok;
        }
        volatile bool      s_enabled = true;
        bool               s_installed = false;

        // ---- log queue: filled on the game thread, written by the plugin thread ----
        constexpr u32 kLogSlots = 16;
        constexpr u32 kLogLen   = 120;
        char          s_logq[kLogSlots][kLogLen];
        volatile u32  s_logHead = 0, s_logTail = 0, s_logDropped = 0;

        __attribute__((format(printf, 1, 2))) void Note(const char *fmt, ...)
        {
            const u32 head = s_logHead;
            if (head - s_logTail >= kLogSlots)
            {
                s_logDropped = s_logDropped + 1;
                return;
            }
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(s_logq[head % kLogSlots], kLogLen, fmt, ap);
            va_end(ap);
            __sync_synchronize();
            s_logHead = head + 1;
        }

        // ---- records (hooks write, the grid's Update consumes) ----
        struct Record
        {
            void *key   = nullptr;   // ListView (tap) or grid (release, drop)
            void *child = nullptr;   // slot widget; only compared until validated
            u8    age   = 0;
        };
        Record s_tap, s_release, s_selfDrop;

        // Carry we started, so the log can say when the game ended it (tab
        // switch, page change). Pointers are only compared.
        void *s_carryGrid = nullptr;
        void *s_carrySrc  = nullptr;
        // What that carry holds, captured at pick-up. Switching the bottom
        // screen tab (chest <-> inventory) hides the carry's grid (0x252B1C
        // exits the move state) and shows another grid, or swaps a grid's data
        // source (0x2527DC, resets the state): either way the game's carry is
        // gone. The Item itself lives in the chest / player arrays and stays
        // put, so a tap on a slot of the new tab still places it ("pending"
        // carry, plugin side; Azahar 2026-10-29: the chest page's tabs are
        // separate grids).
        void *s_carryItem = nullptr;     // Item* in the source container
        void *s_carryDs   = nullptr;     // data source at pick-up
        u8    s_carryKind = 0;           // source slot kind (+0x18C)
        s16   s_carryType = 0, s_carryStack = 0;
        u64   s_carrySeen = 0;           // system tick of the carry grid's last update
        u32   s_otherUpdates = 0;        // updates of other grids since the carry grid's last one
        u64   s_anyGridSeen = 0;         // system tick of the last update of any grid
        bool  s_pending   = false;       // the carry outlived its grid state: any grid's next slot tap places it
        u64   s_pendSeen  = 0;           // system tick of the last grid update while pending
        volatile bool s_pendNotify = false;

        template <typename T>
        inline T Field(void *obj, u32 off) { return *reinterpret_cast<T *>(static_cast<u8 *>(obj) + off); }

        void Set(Record &r, void *key, void *child) { r.key = key; r.child = child; r.age = 0; }
        void Clear(Record &r) { r.key = r.child = nullptr; r.age = 0; }
        void Age(Record &r)
        {
            if (r.key && ++r.age > kRecordMaxAge)
                Clear(r);
        }

        // Carry mode = current state is the move state with mode 1.
        void *CarrySource(void *grid)
        {
            void *state = Field<void *>(grid, kGridState);
            void *move  = Field<void *>(grid, kGridMove);
            if (!state || state != move || Field<u8>(move, kMoveMode) != kModeCarry)
                return nullptr;
            return Field<void *>(move, kMoveSrc);
        }

        bool DragRunning(void *grid)
        {
            void *state = Field<void *>(grid, kGridState);
            void *move  = Field<void *>(grid, kGridMove);
            return state && state == move && Field<u8>(move, kMoveMode) == kModeDrag;
        }

        // Index of `w` in the grid's slot list, -1 if it is not one of its slots.
        int SlotIndex(void *grid, void *w)
        {
            if (!w)
                return -1;
            void **it  = Field<void **>(grid, kGridSlotsBegin);
            void **end = Field<void **>(grid, kGridSlotsEnd);
            for (int i = 0; it && it != end && i < kMaxSlots; ++it, ++i)
                if (*it == w)
                    return i;
            return -1;
        }

        void ItemInfo(void *w, int &type, int &stack)
        {
            void *item = w ? Field<void *>(w, kSlotItem) : nullptr;
            type  = item ? Field<s16>(item, kItemType) : -1;
            stack = item ? Field<s16>(item, kItemStack) : 0;
        }

        // Is the current touch inside `w` (the same test Widget 0x461680 uses)?
        bool TouchInside(void *w)
        {
            void *input = reinterpret_cast<FnInput>(kFnInput)();
            void *touch = input ? reinterpret_cast<FnTouch>(kFnTouch)(input) : nullptr;
            if (!touch)
                return false;
            float pos[4] = {};
            float rect[8] = {};
            reinterpret_cast<FnTouchPos>(kFnTouchPos)(pos, touch);
            void **vt = Field<void **>(w, 0);
            reinterpret_cast<FnRect>(vt[kWidgetVtRect / 4])(rect, w);
            return rect[0] <= pos[0] && pos[0] <= rect[0] + rect[2] &&
                   rect[1] <= pos[1] && pos[1] <= rect[1] + rect[3];
        }

        using FnAccepts = bool (*)(void *item, u32 kind);
        using FnItems   = u32 (*)(void *a, void *b);

        void ClearCarry()
        {
            s_carryGrid = s_carrySrc = s_carryItem = s_carryDs = nullptr;
            s_pending = false;
        }

        // The game's carry disables touch on the whole slot list (0x252A90 ->
        // ListView vt+0x14, every widget's +0x88 = 0): A-carry is meant for the
        // D-pad. That is why v0.5.3 never saw the second tap on hardware. Turn
        // touch back on for a carry we started; the normal-state switch
        // (0x253208) enables it again anyway.
        void EnableListTouch(void *grid)
        {
            void *lv = Field<void *>(grid, kGridLv);
            if (!lv)
                return;
            void **vt = Field<void **>(lv, 0);
            reinterpret_cast<void (*)(void *)>(vt[kWidgetVtEnable / 4])(lv);
        }

        // Pending carry (after a tab switch): the game's own place rules
        // (0x2522AC / 0x252214), with the source slot kind recorded at pick-up.
        void PlacePending(void *grid, void *w, int idx, const char *why)
        {
            void *srcItem = s_carryItem;
            void *dstItem = Field<void *>(w, kSlotItem);
            if (!srcItem || !dstItem || srcItem == dstItem)
            {
                Note("cancelled (%s): pending carry, slot %d is the source", why, idx);
                ClearCarry();
                return;
            }
            const s16 sType = Field<s16>(srcItem, kItemType), sStack = Field<s16>(srcItem, kItemStack);
            if (sType != s_carryType || sStack != s_carryStack || sType <= 0)
            {
                Note("cancelled (%s): pending item changed (%d x%d, was %d x%d)", why, sType, sStack,
                     s_carryType, s_carryStack);
                ClearCarry();
                return;
            }
            const s16 dType = Field<s16>(dstItem, kItemType), dStack = Field<s16>(dstItem, kItemStack);
            const bool ok = reinterpret_cast<FnAccepts>(kFnSlotAccepts)(srcItem, Field<u8>(w, kSlotType))
                         && reinterpret_cast<FnAccepts>(kFnSlotAccepts)(dstItem, s_carryKind);
            if (ok)
            {
                const u32 r = reinterpret_cast<FnItems>(kFnItemMerge)(srcItem, dstItem);
                if (r == 2)
                    reinterpret_cast<FnItems>(kFnItemSwap)(srcItem, dstItem);
            }
            reinterpret_cast<FnGridWidget>(kFnGridToNormal)(grid, w);    // select the target (state is normal)
            Note("%s (%s): pending -> slot %d | before src %d x%d dst %d x%d | after src %d x%d dst %d x%d",
                 ok ? "placed" : "ignored (slot rules refused)", why, idx, sType, sStack, dType, dStack,
                 Field<s16>(srcItem, kItemType), Field<s16>(srcItem, kItemStack),
                 Field<s16>(dstItem, kItemType), Field<s16>(dstItem, kItemStack));
            ClearCarry();
        }

        void Act(void *grid, void *w, const char *why)
        {
            const int idx = SlotIndex(grid, w);
            if (idx < 0)
            {
                Note("ignored (%s): widget is not a slot of grid %p", why, grid);
                return;
            }
            void *state  = Field<void *>(grid, kGridState);
            void *normal = Field<void *>(grid, kGridNormal);
            int type, stack;
            ItemInfo(w, type, stack);

            if (state && state == normal)
            {
                if (s_pending)
                {
                    PlacePending(grid, w, idx, why);
                    return;
                }
                void *helper = Field<void *>(grid, kGridHelper);
                if (type <= 0)
                {
                    Note("ignored (%s): slot %d is empty", why, idx);
                    return;
                }
                if (!helper || !Field<u8>(helper, kHelperEnabled))
                {
                    Note("ignored (%s): slot %d, this grid has carry disabled", why, idx);
                    return;
                }
                reinterpret_cast<FnGridWidget>(kFnGridActivate)(grid, w);
                if (CarrySource(grid) == w)
                {
                    EnableListTouch(grid);
                    s_carryGrid  = grid;
                    s_carrySrc   = w;
                    s_carryItem  = Field<void *>(w, kSlotItem);
                    s_carryDs    = Field<void *>(grid, kGridSource);
                    s_carryKind  = Field<u8>(w, kSlotType);
                    s_carryType  = static_cast<s16>(type);
                    s_carryStack = static_cast<s16>(stack);
                    s_carrySeen  = svcGetSystemTick();
                    s_otherUpdates = 0;
                    s_pending    = false;
                    Note("picked (%s): slot %d, item %d x%d", why, idx, type, stack);
                }
                else
                    Note("ignored (%s): game refused pick-up on slot %d (item %d)", why, idx, type);
                return;
            }

            void *src = CarrySource(grid);
            if (src)
            {
                const int srcIdx = SlotIndex(grid, src);
                if (src == w)
                {
                    reinterpret_cast<FnGridWidget>(kFnGridToNormal)(grid, src);
                    Note("cancelled (%s): tapped the carried slot %d", why, idx);
                }
                else
                {
                    int sType, sStack;
                    ItemInfo(src, sType, sStack);
                    reinterpret_cast<FnGridWidget>(kFnGridActivate)(grid, w);
                    int sType2, sStack2, dType2, dStack2;
                    ItemInfo(src, sType2, sStack2);
                    ItemInfo(w, dType2, dStack2);
                    const bool moved = sType2 != sType || sStack2 != sStack || dType2 != type || dStack2 != stack;
                    Note("%s (%s): slot %d -> %d | before src %d x%d dst %d x%d | after src %d x%d dst %d x%d%s",
                         moved ? "placed" : "ignored", why, srcIdx, idx, sType, sStack, type, stack,
                         sType2, sStack2, dType2, dStack2,
                         moved ? "" : " (slot rules refused)");
                    if (CarrySource(grid))
                        Note("note: still carrying after place on slot %d", idx);
                }
                ClearCarry();
                return;
            }

            if (DragRunning(grid))
                Note("ignored (%s): slot %d, touch drag in progress", why, idx);
            else
                Note("ignored (%s): slot %d, grid state %p not normal/carry", why, idx, state);
        }

        void Hooked_LvChildTap(void *lv, void *child)
        {
            hook::FunctionHook::CallOriginal<void>(lv, child);
            // Stock (0x48C8DC..0x48C974) only fires the tap when the touch is
            // inside the ListView itself: a slot scrolled out of view can sit
            // under a tab or header. Record exactly the taps the game accepts.
            if (s_enabled && lv && child && TouchInside(lv))
                Set(s_tap, lv, child);
        }

        void Hooked_GridRelease(void *grid, void *child)
        {
            // Stock first: the state's touch-select (normal) or nothing (move).
            hook::FunctionHook::CallOriginal<void>(grid, child);
            if (!s_enabled || !grid || !child)
                return;
            // A drag is still running here (the helper's Drop comes after this
            // signal): leave it to the drop record or to the game.
            if (DragRunning(grid))
                return;
            // Carry: lift anywhere on a slot places. Normal: the press has to
            // have started on this slot too (no pick-up at the end of a swipe).
            const bool carrying = CarrySource(grid) != nullptr;
            if (!carrying && !Field<u8>(child, kWidgetPressedHere))
                return;
            if (!TouchInside(child))
                return;
            Set(s_release, grid, child);
        }

        void Hooked_GridDropped(void *grid, void *helper)
        {
            void *src = helper ? Field<void *>(helper, kHelperSource) : nullptr;
            void *dst = helper ? Field<void *>(helper, kHelperTarget) : nullptr;
            const bool selfDrop = s_enabled && grid && src && src == dst && DragRunning(grid);
            // Stock: the move state places src on dst (on itself: a no-op merge)
            // and returns to normal; the helper then clears its drag.
            hook::FunctionHook::CallOriginal<void>(grid, helper);
            if (selfDrop)
                Set(s_selfDrop, grid, src);
        }

        void Hooked_GridUpdate(void *grid, float dt)
        {
            hook::FunctionHook::CallOriginal<void>(grid, dt);
            Age(s_tap);
            Age(s_release);
            Age(s_selfDrop);
            if (!grid)
                return;
            if (!s_enabled)
            {
                Clear(s_tap);
                Clear(s_release);
                Clear(s_selfDrop);
                return;
            }

            // Carry bookkeeping before any action (see s_carryItem).
            const u64 now = svcGetSystemTick();
            // No grid updated for a while: the inventory / chest was closed.
            // Whatever we were carrying stays where it is; forget it, so a
            // later page never picks up a stale carry (audit, v0.5.4).
            if ((s_carryGrid || s_pending) && now - s_anyGridSeen > kPendingTicks)
            {
                Note("carry dropped: inventory closed");
                ClearCarry();
            }
            s_anyGridSeen = now;
            void *src = CarrySource(grid);
            void *input = reinterpret_cast<FnInput>(kFnInput)();
            const bool pressedB = input && reinterpret_cast<FnPressed>(kFnPressed)(input, kButtonB);
            if (s_carryGrid && !s_pending)
            {
                if (grid == s_carryGrid)
                {
                    s_carrySeen = now;
                    s_otherUpdates = 0;
                    if (src != s_carrySrc)
                    {
                        // Same grid, data source swapped: keep it. Anything
                        // else (A placed it, B, a new carry) ends it.
                        if (!src && Field<void *>(grid, kGridSource) != s_carryDs && s_carryItem)
                            s_pending = true;
                        else
                        {
                            Note("carry ended by the game (A button or page change)");
                            ClearCarry();
                        }
                    }
                }
                else if (++s_otherUpdates >= kHiddenUpdates && now - s_carrySeen > kHiddenTicks)
                    s_pending = true;           // another grid runs and ours stopped: tab switched
                if (s_pending)
                {
                    s_pendSeen = now;
                    s_pendNotify = true;
                    Note("carry kept across a tab switch: item %d x%d, tap a slot to place it",
                         s_carryType, s_carryStack);
                }
            }
            else if (s_pending)
            {
                // Dropped when the inventory was closed (no grid updated for a
                // while), on B, or when the game starts its own carry.
                const bool expired = now - s_pendSeen > kPendingTicks;
                if (expired || pressedB || src)
                {
                    Note("pending carry dropped (%s)", expired ? "inventory closed" : pressedB ? "B" : "game carry");
                    ClearCarry();
                }
                else
                    s_pendSeen = now;
            }

            // One action per touch: a quick tap records both "tap" and
            // "release" for the same slot.
            void *lv = Field<void *>(grid, kGridLv);
            void *child = nullptr;
            const char *why = nullptr;
            if (s_selfDrop.key == grid)       { child = s_selfDrop.child; why = "held"; }
            else if (lv && s_tap.key == lv)   { child = s_tap.child;      why = "tap"; }
            else if (s_release.key == grid)   { child = s_release.child;  why = "release"; }
            if (s_selfDrop.key == grid) Clear(s_selfDrop);
            if (lv && s_tap.key == lv)  Clear(s_tap);
            if (s_release.key == grid)  Clear(s_release);

            if (child)
            {
                Act(grid, child, why);
                return;
            }

            if (src && src == s_carrySrc && grid == s_carryGrid && pressedB)
            {
                reinterpret_cast<FnGridWidget>(kFnGridToNormal)(grid, src);   // item never left its slot
                Note("cancelled (B): carried slot %d", SlotIndex(grid, src));
                ClearCarry();
            }
        }
    }

    void Install()
    {
        const hook::Result a = s_tapHook.Install(kFnLvChildTap, reinterpret_cast<void *>(Hooked_LvChildTap),
                                                 kLvChildTapInsn);
        const hook::Result b = s_releaseHook.Install(kFnGridRelease, reinterpret_cast<void *>(Hooked_GridRelease),
                                                     kGridReleaseInsn);
        const hook::Result c = s_droppedHook.Install(kFnGridDropped, reinterpret_cast<void *>(Hooked_GridDropped),
                                                     kGridDroppedInsn);
        const hook::Result d = s_updateHook.Install(kFnGridUpdate, reinterpret_cast<void *>(Hooked_GridUpdate),
                                                    kGridUpdateInsn);
        const hook::Result t = s_touchReadHook.Install(kFnTouchRead, reinterpret_cast<void *>(Hooked_TouchRead),
                                                       kTouchReadInsn);
        s_installed = a == hook::Result::Ok && b == hook::Result::Ok && c == hook::Result::Ok &&
                      d == hook::Result::Ok;
        // Partial installs are harmless (each hook calls the game first and
        // only records; actions need the Update hook), but report it.
        Note("hooks: tap %s, release %s, dropped %s, update %s, test touch %s", hook::ResultString(a),
             hook::ResultString(b), hook::ResultString(c), hook::ResultString(d), hook::ResultString(t));
    }

    void FlushLog()
    {
        if (s_pendNotify)
        {
            s_pendNotify = false;
            CTRPluginFramework::OSD::Notify("Item still in hand: tap a slot to put it there (B cancels)");
        }
        while (s_logTail != s_logHead)
        {
            __sync_synchronize();
            const u32 tail = s_logTail;
            perf::Log(std::string("inv tap: ") + s_logq[tail % kLogSlots]);
            __sync_synchronize();   // slot fully read before the producer may reuse it
            s_logTail = tail + 1;
        }
        if (s_logDropped)
        {
            perf::Log("inv tap: " + std::to_string(s_logDropped) + " log lines dropped (queue full)");
            s_logDropped = 0;
        }
    }

    void SetEnabled(bool on) { s_enabled = on; }
    bool Enabled()           { return s_enabled; }
    bool Installed()         { return s_installed; }
}
