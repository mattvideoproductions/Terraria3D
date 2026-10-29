#include "version_check.hpp"
#include "game_layout.hpp"
#include "memory.hpp"
#include "text_page_crcs.hpp"

#include <CTRPluginFramework.hpp>

using CTRPluginFramework::Process;

namespace t3d::version
{
    static Status s_status = Status::NotChecked;
    static u32    s_badAddr = 0;

    // Plain table-driven CRC-32 (same as zlib.crc32), streaming form:
    // start with crc = 0xFFFFFFFF, finish with ~crc.
    static u32 s_table[256];

    static void Crc32Init()
    {
        for (u32 i = 0; i < 256; i++)
        {
            u32 c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            s_table[i] = c;
        }
    }

    static u32 Crc32Update(u32 crc, const u8 *p, u32 len)
    {
        while (len--)
            crc = s_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
        return crc;
    }

    static u32 s_foreignPages = 0;

    static bool PageIsProtected(u32 pageAddr)
    {
        for (u32 a : t3d::game::kProtectedAddrs)
            if ((a & ~(t3d::game::kTextPageSize - 1)) == pageAddr)
                return true;
        return false;
    }

    // Compare every 4 KiB page of .text against the v5.6.0 table. The plugin
    // loader / CTRPF / emulators hook a few system-call stubs before we run
    // (hardware: 1 word; Azahar: 4 words), so a handful of "foreign" pages is
    // tolerated - but never a page that contains one of our own patch sites.
    // Returns false (and sets s_badAddr) if the build does not match.
    static bool TextPagesMatch()
    {
        using namespace t3d::game;
        Crc32Init();
        s_foreignPages = 0;
        for (u32 i = 0; i < kTextPageCount; i++)
        {
            const u32 page = kTextStart + i * kTextPageSize;
            const u32 len = (page + kTextPageSize > kTextEnd) ? kTextEnd - page : kTextPageSize;
            const u32 crc = ~Crc32Update(0xFFFFFFFFu, reinterpret_cast<const u8 *>(page), len);
            if (crc == kTextPageCrc[i])
                continue;
            if (!s_badAddr)
                s_badAddr = page;
            if (PageIsProtected(page) || ++s_foreignPages > kMaxForeignPages)
            {
                s_badAddr = page;
                return false;
            }
        }
        return true;
    }

    Status Verify(bool fullTextCrc)
    {
        using namespace t3d::game;
        mem::SetPatchingAllowed(false);
        s_badAddr = 0;

        if (Process::GetTitleID() != kTitleId)
            return s_status = Status::WrongTitle;

        // Cheap check first: a handful of build-specific words.
        for (const SigWord &w : kSignature)
        {
            if (*reinterpret_cast<const volatile u32 *>(w.addr) != w.value)
            {
                s_badAddr = w.addr;
                return s_status = Status::WrongSignature;
            }
        }

        // Strong check: per-page CRC-32 of the whole .text (about 5 MB; once at boot).
        if (fullTextCrc && !TextPagesMatch())
            return s_status = Status::WrongCrc;

        mem::SetPatchingAllowed(true);
        return s_status = Status::Ok;
    }

    Status GetStatus()          { return s_status; }
    bool   IsSupported()        { return s_status == Status::Ok; }
    u32    ForeignPageCount()   { return s_foreignPages; }
    u32    FirstMismatchAddr()  { return s_badAddr; }

    const char *StatusString(Status s)
    {
        switch (s)
        {
            case Status::NotChecked:     return "not checked";
            case Status::Ok:             return "OK (USA v5.6.0)";
            case Status::WrongTitle:     return "wrong title (not Terraria USA)";
            case Status::WrongSignature: return "unsupported update version";
            case Status::WrongCrc:       return "code.bin modified / unknown build";
        }
        return "?";
    }
}
