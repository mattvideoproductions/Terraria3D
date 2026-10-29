#pragma once
#include <3ds/types.h>

namespace t3d::version
{
    enum class Status
    {
        NotChecked,
        Ok,
        WrongTitle,      // not Terraria USA
        WrongSignature,  // instruction / literal words differ -> other update
        WrongCrc,        // too many changed .text pages, or one of our patch pages changed
    };

    // Run the checks (title ID, signature words, optional full .text CRC32).
    // On success this also unlocks t3d::mem patching; on failure patching stays locked.
    Status  Verify(bool fullTextCrc = true);

    Status      GetStatus();
    bool        IsSupported();
    const char *StatusString(Status s);
    u32         ForeignPageCount();  // .text pages changed by loader/CTRPF hooks (tolerated)
    u32         FirstMismatchAddr(); // first bad signature word / first changed .text page, 0 if none
}
