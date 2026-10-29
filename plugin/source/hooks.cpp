#include "hooks.hpp"
#include "memory.hpp"

using namespace CTRPluginFramework;

namespace t3d::hook
{
    static Result Precheck(u32 target, u32 expectedInsn)
    {
        if (!mem::PatchingAllowed())
            return Result::Locked;
        if ((target & 3) || mem::RegionOf(target, 8) != mem::Region::Text)
            return Result::BadAddress;
        if (expectedInsn != 0)
        {
            u32 cur;
            if (!mem::Read32(target, cur) || cur != expectedInsn)
                return Result::UnexpectedCode;
        }
        return Result::Ok;
    }

    Result FunctionHook::Install(u32 target, void *callback, u32 expectedInsn)
    {
        if (_installed)
            return Result::AlreadyInstalled;

        Result r = Precheck(target, expectedInsn);
        if (r != Result::Ok)
            return r;

        // MITM mode: the first instruction is replaced by a branch to a CTRPF
        // stub that calls `callback`. OriginalFunction() executes the overwritten
        // instruction and continues at target + 4 (trampoline).
        _hook.InitializeForMitm(target, reinterpret_cast<u32>(callback));
        _ctrpfResult = _hook.Enable();
        if (_ctrpfResult != HookResult::Success)
            return Result::CtrpfError;

        _target = target;
        _installed = true;
        return Result::Ok;
    }

    Result FunctionHook::Remove()
    {
        if (!_installed)
            return Result::Ok;
        _ctrpfResult = _hook.Disable();
        if (_ctrpfResult != HookResult::Success)
            return Result::CtrpfError;
        _installed = false;
        return Result::Ok;
    }

    Result SlotHook::Install(u32 target, void (*callback)(void), u32 expectedInsn)
    {
        if (_installed)
            return Result::AlreadyInstalled;

        Result r = Precheck(target, expectedInsn);
        if (r != Result::Ok)
            return r;

        // HOOK_DEFAULT_PARAMS: run the overwritten instruction, call the callback
        // with LR restored afterwards, return to target + 4.
        _hook.Initialize(target, reinterpret_cast<u32>(callback));
        _ctrpfResult = _hook.Enable();
        if (_ctrpfResult != HookResult::Success)
            return Result::CtrpfError;

        _installed = true;
        return Result::Ok;
    }

    Result ReplaceFunction(u32 target, void *replacement, u32 expectedInsn)
    {
        Result r = Precheck(target, expectedInsn);
        if (r != Result::Ok)
            return r;
        return mem::PatchAbsJump(target, reinterpret_cast<u32>(replacement)) ? Result::Ok
                                                                             : Result::BadAddress;
    }

    const char *ResultString(Result r)
    {
        switch (r)
        {
            case Result::Ok:               return "ok";
            case Result::Locked:           return "locked (version check failed)";
            case Result::BadAddress:       return "bad address";
            case Result::UnexpectedCode:   return "unexpected instruction at target";
            case Result::AlreadyInstalled: return "already installed";
            case Result::CtrpfError:       return "CTRPF hook error";
        }
        return "?";
    }
}
