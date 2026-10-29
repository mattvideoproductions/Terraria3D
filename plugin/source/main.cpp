// Terraria 3D mod - Luma3DS 3GX plugin (CTRPluginFramework)
#include <CTRPluginFramework.hpp>
#include <string>
#include <vector>

#include "features.hpp"
#include "game_layout.hpp"
#include "hooks.hpp"
#include "inventory_tap.hpp"
#include "memory.hpp"
#include "perf.hpp"
#include "perf_fixes.hpp"
#include "perf_probes.hpp"
#include "stereo.hpp"
#include "stereo_layers.hpp"
#include "stereo_present.hpp"
#include "ui_theme.hpp"
#include "version_check.hpp"

#define T3D_VERSION_STR "0.5.4"
#define T3D_NAME        "Terraria 3D mod v" T3D_VERSION_STR

namespace CTRPluginFramework
{
    // ---------------------------------------------------------------------
    // Runs once, before the game starts executing. Verify the game build and
    // apply early patches here (only when the check passed).
    // ---------------------------------------------------------------------
    void PatchProcess(FwkSettings &settings)
    {
        settings.WaitTimeToBoot = Seconds(3);

        t3d::version::Verify(/*fullTextCrc=*/true);

        if (t3d::version::IsSupported())
        {
            t3d::stereo::present::Install();
            t3d::features::Install();
            t3d::perf_fixes::Install();
            t3d::perf::probes::Install();
            t3d::inventory_tap::Install();
        }
    }

    // ---------------------------------------------------------------------
    // Menu state. Toggle entries carry a no-op game function so CTRPF shows a
    // checkbox; PollMenu() reads them every frame on the plugin thread, so
    // switching something OFF is always seen (a game function stops being
    // called once its entry is disabled).
    // ---------------------------------------------------------------------
    static void NoOp(MenuEntry *) {}

    static MenuEntry *e_stereo, *e_perLayer, *e_invert, *e_stereoDebug, *e_fps, *e_slider, *e_perfLog;
    static MenuEntry *e_invTap;
    static MenuEntry *e_snapPx, *e_edgeFill, *e_bgCull, *e_bgViewport, *e_bgMerge, *e_bgFastLight, *e_noCatchUp;
    static u32 s_statsSeq = 0;
    static std::string s_bgStats = "bg -";

    static void PollMenu()
    {
        namespace sp = t3d::stereo::present;
        sp::SetEnabled(e_stereo->IsActivated());
        sp::SetPerLayer(e_perLayer->IsActivated());
        sp::SetInvert(e_invert->IsActivated());
        t3d::stereo::layers::SetSnapPixels(e_snapPx->IsActivated());
        t3d::stereo::layers::SetEdgeFill(e_edgeFill->IsActivated());
        t3d::perf_fixes::SetBackdropCull(e_bgCull->IsActivated());
        t3d::perf_fixes::SetViewportCull(e_bgViewport->IsActivated());
        t3d::perf_fixes::SetMergeRuns(e_bgMerge->IsActivated());
        t3d::perf_fixes::SetFastLighting(e_bgFastLight->IsActivated());
        t3d::perf_fixes::SetNoCatchUp(e_noCatchUp->IsActivated());
        t3d::inventory_tap::SetEnabled(e_invTap->IsActivated());
        t3d::inventory_tap::FlushLog();   // tap action lines queued on the game thread

        if (e_perfLog->IsActivated())
        {
            // Backdrop counters are per pass since the last window
            const u32 seq = t3d::perf::Last().seq;
            if (seq != s_statsSeq)
            {
                s_statsSeq = seq;
                s_bgStats = t3d::perf_fixes::TakeStats();
            }
            std::string ctx = Utils::Format("3D %s slider %d%% perlayer %d safe %d snap %d preset %d zoom %d",
                                            sp::CurrentDisparity() || stereo_get_slider() > 0.01f ? "on" : "off",
                                            (int)(stereo_get_slider() * 100.0f + 0.5f),
                                            e_perLayer->IsActivated() ? 1 : 0,
                                            t3d::stereo::layers::SafeMode(),
                                            e_snapPx->IsActivated() ? 1 : 0,
                                            t3d::stereo::layers::Preset(),
                                            t3d::features::Zoom2x() ? 2 : 1);
            ctx += Utils::Format(" | %s vpc %d mrg %d fl %d nocu %d", s_bgStats.c_str(),
                                 e_bgViewport->IsActivated() ? 1 : 0, e_bgMerge->IsActivated() ? 1 : 0,
                                 e_bgFastLight->IsActivated() ? 1 : 0, t3d::perf_fixes::NoCatchUp() ? 1 : 0);
            t3d::perf::LogWindowIfNew(ctx + " | " + t3d::features::CstickStatus());
        }
    }

    static void StereoTestModeMenuFunc(MenuEntry *)
    {
        namespace sp = t3d::stereo::present;
        std::vector<std::string> options;
        for (int i = 0; i < sp::TestModeCount(); i++)
            options.push_back(Utils::Format("%d: %s", i, sp::TestModeName(i)));
        Keyboard kb(Utils::Format("Stereo test mode (current %d).", sp::TestMode()), options);
        int choice = kb.Open();
        if (choice >= 0)
            sp::SetTestMode(choice);
    }

    static void StereoSafeModeMenuFunc(MenuEntry *)
    {
        namespace sl = t3d::stereo::layers;
        std::vector<std::string> options;
        for (int i = 0; i < sl::SafeModeCount(); i++)
            options.push_back(Utils::Format("%d: %s", i, sl::SafeModeName(i)));
        Keyboard kb(Utils::Format("Stereo safe mode (current %d).\n"
                                  "For A/B tests of glitches seen with 3D on.", sl::SafeMode()), options);
        int choice = kb.Open();
        if (choice >= 0)
            sl::SetSafeMode(choice);
    }

    static void PopOutMenuFunc(MenuEntry *)
    {
        static const float kScales[] = { 0.0f, 1.0f, 1.75f };
        Keyboard kb(Utils::Format("Pop-out effects (particles, damage numbers,\ngore, projectiles, UI). Current: x%.2f",
                                  t3d::stereo::layers::PopScale()),
                    { "Off (nothing in front of the screen)", "Normal", "Extra" });
        int choice = kb.Open();
        if (choice >= 0 && choice < 3)
            t3d::stereo::layers::SetPopScale(kScales[choice]);
    }

    static void DepthPresetMenuFunc(MenuEntry *)
    {
        namespace sl = t3d::stereo::layers;
        std::vector<std::string> options;
        for (int i = 0; i < sl::PresetCount(); i++)
            options.push_back(sl::PresetName(i));
        Keyboard kb(Utils::Format("Depth preset: what sits where.\nCurrent: %s", sl::PresetName(sl::Preset())),
                    options);
        int choice = kb.Open();
        if (choice >= 0)
            sl::SetPreset(choice);
    }

    static void UiThemeMenuFunc(MenuEntry *)
    {
        namespace ut = t3d::ui_theme;
        std::vector<std::string> options;
        for (int i = 0; i < ut::ThemeCount(); i++)
            options.push_back(ut::ThemeName(i));
        Keyboard kb(Utils::Format("Bottom-screen UI colours.\nCurrent: %s\n%s", ut::ThemeName(ut::Theme()), ut::Status()),
                    options);
        int choice = kb.Open();
        if (choice >= 0)
            ut::SetTheme(choice);
    }

    static void DepthStrengthMenuFunc(MenuEntry *)
    {
        Keyboard kb(Utils::Format("Max separation in pixels at full slider (0-24).\nCurrent: %d",
                                  t3d::stereo::present::MaxDisparity()));
        kb.IsHexadecimal(false);
        u32 value = 0;
        if (kb.Open(value) == 0)
            t3d::stereo::present::SetMaxDisparity(static_cast<int>(value));
    }

    // ---------------------------------------------------------------------
    // OSD overlays. Text goes on the bottom screen: it stays readable with 3D
    // on (and when something goes wrong with the top screen).
    // ---------------------------------------------------------------------
    static void DrawLines(const Screen &screen, const std::string &text, int y)
    {
        size_t start = 0;
        while (start <= text.size())
        {
            size_t nl = text.find('\n', start);
            screen.Draw(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start),
                        2, y, Color::White, Color::Black);
            y += 10;
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
    }

    static bool DrawStatus(const Screen &screen)
    {
        if (screen.IsTop)
            return false;

        std::string text;
        if (e_fps && e_fps->IsActivated())
            text += t3d::perf::SummaryLine() + "\n";
        if (e_slider && e_slider->IsActivated())
            text += Utils::Format("3D slider: %d%%\n", (int)(stereo_get_slider() * 100.0f + 0.5f));
        if (t3d::features::ShowCstickDebug())
            text += t3d::features::CstickStatus() + "\n";
        if (e_stereoDebug && e_stereoDebug->IsActivated())
            text += t3d::stereo::present::DebugText() + "\n";
        if (text.empty())
            return false;
        DrawLines(screen, text, 2);
        return true;
    }

    static void StatusMenuFunc(MenuEntry *)
    {
        using namespace t3d;
        std::string msg;
        msg += Utils::Format("Title ID: %016llX\n", Process::GetTitleID());
        msg += Utils::Format("Title version: %u\n", Process::GetVersion());
        msg += std::string("Version check: ") + version::StatusString(version::GetStatus()) + "\n";
        msg += Utils::Format("Hooked .text pages: %lu (max %lu)\n",
                             version::ForeignPageCount(), game::kMaxForeignPages);
        msg += std::string("Patching: ") + (mem::PatchingAllowed() ? "enabled" : "LOCKED") + "\n";
        msg += std::string("Stereo: ") + stereo::present::StateString(stereo::present::GetState()) + "\n";
        msg += std::string("Per-layer: ") + (!stereo::layers::Installed() ? "hooks NOT installed"
                 : stereo::layers::RightTargetFailed() ? "right target alloc failed"
                 : Utils::Format("ok, %lu runs replayed, %lu edge fills last frame",
                                stereo::layers::ReplayedRunsLastFrame(), stereo::layers::EdgeFillsLastFrame())) + "\n";
        msg += perf::SummaryLine() + "\n";
        msg += std::string("Log: ") + perf::LogPath();
        MessageBox(T3D_NAME, msg)();
    }

    // ---------------------------------------------------------------------
    int main(void)
    {
        PluginMenu *menu = new PluginMenu("Terraria 3D mod", 0, 5, 4,
            "Terraria 3D mod v" T3D_VERSION_STR "\n"
            "Stereo 3D, 2x zoom, cheats and creative mode\nfor Terraria 3DS USA v5.6.0.");
        menu->SynchronizeWithFrame(true);
        menu->OnOpening = []() -> bool
        {
            t3d::stereo::present::SyncRightBuffersForMenu();
            t3d::features::OnMenuOpening();
            return true;
        };
        menu->OnClosing = []()
        {
            t3d::features::OnMenuClosing();
        };

        t3d::perf::Log("---- " T3D_NAME " started; version check: " +
                       std::string(t3d::version::StatusString(t3d::version::GetStatus())));
        if (t3d::version::IsSupported())
            OSD::Notify(T3D_NAME " loaded", Color::LimeGreen);
        else
            OSD::Notify(std::string(T3D_NAME ": ") +
                        t3d::version::StatusString(t3d::version::GetStatus()) +
                        " - no patches applied", Color::Red);

        menu->Append(new MenuEntry(T3D_NAME, nullptr, StatusMenuFunc,
                                   "Plugin status, game version check and performance."));

        // ---- Stereo 3D ----
        MenuFolder *stereo = new MenuFolder("Stereo 3D");
        stereo->Append(e_stereo = new MenuEntry("Stereo 3D enabled", NoOp,
                                                "Real stereoscopic 3D on the top screen. Follows the 3D slider."));
        e_stereo->Enable();
        stereo->Append(e_perLayer = new MenuEntry("Per-layer depth", NoOp,
                                                  "Sky far back, walls behind tiles, particles and HUD in front.\n"
                                                  "Off = whole picture at one depth. Costs more performance."));
        // Default since v0.3.2 on New 3DS (~60 fps on hardware, v0.3.0 log). Old 3DS
        // starts with flat 3D: per-layer replay costs too much without the faster CPU.
        if (System::IsNew3DS())
            e_perLayer->Enable();
        stereo->Append(new MenuEntry("Depth preset", nullptr, DepthPresetMenuFunc,
                                     "Terrarium Window: all behind the screen (default).\n"
                                     "Diorama: play plane on the screen, effects pop out.\n"
                                     "Pop-out: world in front.\n"
                                     "Tight walls: no wall gaps. Cinematic: deep + big pops.\n"
                                     "Caves, Builder (shallow, precise), Combat (enemies forward)."));
        stereo->Append(new MenuEntry("Depth strength", nullptr, DepthStrengthMenuFunc,
                                     "Maximum left/right separation in pixels at full slider."));
        stereo->Append(new MenuEntry("Pop-out effects", nullptr, PopOutMenuFunc,
                                     "How far particles, damage numbers, gore and projectiles\njump out of the screen."));
        stereo->Append(e_invert = new MenuEntry("Invert depth", NoOp,
                                                "Flip between behind-the-screen and out-of-the-screen."));
        MenuFolder *diag = new MenuFolder("Diagnostics");
        diag->Append(e_stereoDebug = new MenuEntry("Stereo debug (bottom screen)", NoOp,
                                                   "Buffer addresses and display mode."));
        diag->Append(new MenuEntry("Stereo test mode", nullptr, StereoTestModeMenuFunc,
                                   "Where the right-eye image lives."));
        diag->Append(new MenuEntry("Stereo safe mode", nullptr, StereoSafeModeMenuFunc,
                                   "0 = normal per-layer depth.\n"
                                   "1 = right eye replayed with no shift (flat picture):\n"
                                   "    glitches here = the replay itself is at fault.\n"
                                   "2 = flat shift of the left image, no replay (stable)."));
        diag->Append(e_snapPx = new MenuEntry("Whole-pixel layer shifts", NoOp,
                                              "Round each layer's shift to whole pixels so both eyes\n"
                                              "sample textures identically. On by default."));
        e_snapPx->Enable();
        diag->Append(e_edgeFill = new MenuEntry("Fill layer edges", NoOp,
                                                "Background layers shifted for depth would leave a bare\n"
                                                "strip at the left screen edge in the right eye. This\n"
                                                "draws a mirrored sliver of the layer there. On by default."));
        e_edgeFill->Enable();
        stereo->Append(diag);
        menu->Append(stereo);

        menu->Append(new MenuEntry("UI theme", nullptr, UiThemeMenuFunc,
                                   "Bottom-screen menu colours. Dusk = muted grey-purple (default),\n"
                                   "Stock = the game's original colours (restored exactly)."));

        menu->Append(e_invTap = new MenuEntry("Tap to move items (Vita style)", NoOp,
                                              "Inventory and chest grids: tap an item to pick it up,\n"
                                              "tap another slot to place, merge or swap it. Tap the\n"
                                              "same slot or press B to put it back. Drag still works."));
        e_invTap->Enable();

        // ---- Zoom, cheats, creative ----
        t3d::features::BuildMenu(*menu);

        // ---- Performance ----
        MenuFolder *perfFolder = new MenuFolder("Performance");
        perfFolder->Append(e_fps = new MenuEntry("Show FPS (bottom screen)", NoOp,
                                                 "Average/minimum FPS, top-screen CPU time and 3D cost (1 s windows)."));
        perfFolder->Append(e_slider = new MenuEntry("Show 3D slider value", NoOp, "Bottom screen."));
        perfFolder->Append(e_perfLog = new MenuEntry("Log performance to SD", NoOp,
                                                     "Appends one line per second to\n/luma/plugins/000400000016A900/t3d_log.txt"));
        e_perfLog->Enable();
        perfFolder->Append(e_bgCull = new MenuEntry("Cave backdrop culling (faster)", NoOp,
                                                    "Skips cave background pieces that solid blocks cover\n"
                                                    "completely. Uncheck to draw them all (stock)."));
        e_bgCull->Enable();
        perfFolder->Append(e_bgViewport = new MenuEntry("Skip off-screen backdrop (faster)", NoOp,
                                                        "Cave background pieces outside the screen are not\n"
                                                        "drawn (the game draws a wide margin). Biggest gain\n"
                                                        "with 2x zoom. No visible change."));
        e_bgViewport->Enable();
        perfFolder->Append(e_bgMerge = new MenuEntry("Merge backdrop strips (faster)", NoOp,
                                                     "Draws runs of identical cave background pieces as\n"
                                                     "one sprite. Same picture, fewer draw calls."));
        e_bgMerge->Enable();
        perfFolder->Append(e_bgFastLight = new MenuEntry("Fast backdrop lighting", NoOp,
                                                         "Also merges pieces with nearly equal light. Faster in\n"
                                                         "lit caverns; cave background light gets slightly\n"
                                                         "blockier. On by default (about +10 fps in caves)."));
        e_bgFastLight->Enable();
        perfFolder->Append(e_noCatchUp = new MenuEntry("No catch-up (1 tick per frame)", NoOp,
                                                       "When a frame runs late the game normally runs 2 logic\n"
                                                       "ticks to catch up, which makes the next frame late too.\n"
                                                       "On: never more than 1 tick per frame. Smoother frame\n"
                                                       "rate, but the game runs in slow motion when it drops\n"
                                                       "below 60. Off by default."));
        menu->Append(perfFolder);

        *menu += PollMenu;
        OSD::Run(DrawStatus);

        menu->Run();

        // Run() only returns while the game process is shutting down. Deleting
        // the menu there crashed in ~MenuFolderImpl on hardware (v0.2.1 dump),
        // and the OS frees everything anyway, so leave it.
        return 0;
    }
}
