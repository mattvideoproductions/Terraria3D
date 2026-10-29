#pragma once
// Function hooking on top of CTRPluginFramework's Hook (MITM / trampoline mode).
//
// Usage:
//     static t3d::hook::FunctionHook s_drawHook;
//     static void Hooked_Draw(void *self, int layer) {
//         // ... before ...
//         t3d::hook::FunctionHook::CallOriginal<void>(self, layer);   // trampoline
//         // ... after ...
//     }
//     s_drawHook.Install(0x00123456, (void *)Hooked_Draw, 0xE92D4FF0 /*expected 1st insn*/);
//
// CallOriginal() must be called from inside the hook callback (it uses the
// current hook context to run the overwritten instruction and jump to target+4).
// Install() refuses if the version check failed, the target is outside game
// .text, or the expected first instruction does not match.
#include <3ds/types.h>
#include <CTRPluginFramework.hpp>

namespace t3d::hook
{
    enum class Result
    {
        Ok,
        Locked,             // version check not passed
        BadAddress,         // target outside game .text
        UnexpectedCode,     // first instruction != expected
        AlreadyInstalled,
        CtrpfError,         // see LastCtrpfResult()
    };

    class FunctionHook
    {
    public:
        // expectedInsn == 0 disables the per-hook instruction check
        Result  Install(u32 target, void *callback, u32 expectedInsn);
        Result  Remove();
        bool    Installed() const { return _installed; }
        u32     Target() const { return _target; }
        CTRPluginFramework::HookResult LastCtrpfResult() const { return _ctrpfResult; }

        template <typename R, typename... Args>
        static R CallOriginal(Args... args)
        {
            return CTRPluginFramework::HookContext::GetCurrent().OriginalFunction<R>(args...);
        }

    private:
        CTRPluginFramework::Hook       _hook;
        CTRPluginFramework::HookResult _ctrpfResult = CTRPluginFramework::HookResult::Success;
        u32     _target = 0;
        bool    _installed = false;
    };

    // Plain call-out hook for a free slot inside a function (e.g. a `nop` in the
    // main loop). `callback` is called as a normal void(void) function; LR and
    // callee-saved registers are preserved, r0-r3/r12 are not, so only use slots
    // where those are dead (e.g. right before a `bl`). The overwritten
    // instruction runs before the callback, execution resumes at target + 4.
    class SlotHook
    {
    public:
        Result  Install(u32 target, void (*callback)(void), u32 expectedInsn);
        bool    Installed() const { return _installed; }
        CTRPluginFramework::HookResult LastCtrpfResult() const { return _ctrpfResult; }

    private:
        CTRPluginFramework::Hook       _hook;
        CTRPluginFramework::HookResult _ctrpfResult = CTRPluginFramework::HookResult::Success;
        bool    _installed = false;
    };

    // Replace a whole function with `replacement` (no way back to the original).
    // Writes an 8-byte absolute jump; checks `expectedInsn` at target first.
    Result  ReplaceFunction(u32 target, void *replacement, u32 expectedInsn);

    const char *ResultString(Result r);
}
