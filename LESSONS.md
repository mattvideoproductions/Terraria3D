# Lessons from the Terraria 3D mod (for every 3DS plugin port)

These are the mistakes made and the discoveries along the way, v0.2.0 to v0.5.0. Read this before writing hardware-facing code.

## Hardware crashes and black screens (the emulator passed every one of these)
1. **Instruction cache.** v0.2.0 crashed the kernel at boot. `svcInvalidateInstructionCacheRange` runs in whatever process is current (it was i2c), which caused a data abort. Always use `svcInvalidateEntireInstructionCache`, as CTRPF does.
2. **Lazy VRAM allocation.** Allocating right-eye buffers in-world failed on hardware because game textures fill VRAM. Emulator tests had the 3D factor at 100 from boot, which hid this. Reserve stereo memory early (at the title screen), and always test with the slider at 0 at boot, raising it in-world.
3. **FCRAM display buffers.** Buffers from the game's own allocator showed BLACK on hardware (likely non-linear or cached memory). Use VRAM or proper linear memory.
4. **Plugin memory.** The plugin's ~5 MiB came out of game memory, so the game's own texture allocation failed. The result was an svcBreak when the world map opened. `UsePrivateMemory: true` in `.plgInfo` fixed it; keep it on. A similar dump recurred once afterwards, so it's not proven fully fixed.
5. **Process exit.** Deleting the CTRPF menu after `menu->Run()` returns crashed in `~MenuFolderImpl`. Don't free anything at shutdown.
6. **Input.** Circle Pad Pro / C-stick input through the game's own ExtraPad (ir:USER) read "connected" but always gave 0,0 on hardware, while it worked in Azahar. Fix: open ir:rst in the plugin (`irrstInit`/`irrstScanInput`/`irrstCstickRead`, raw ±~140). Never gate on the game's "connected" flag.
7. **Display LUT.** The colour LUT registers (PA 0x10400480/484 top, 0x10400580/584 bottom, accessed as PA|0x80000000) aren't emulated by Azahar. Anything display-controller-level must be tested on hardware.
8. **Suspected (being investigated in v0.5).** White tiles and outlines in dark areas when per-layer stereo replays draw runs for the right eye. Likely stale or recycled vertex/colour data or missing state at replay time. Expect GPU-reads-after-CPU-overwrites problems that the emulator never shows: flush the data cache, and don't reuse memory the GPU will still read later in the frame.

## Emulator limits (Azahar)
- It hides cache, VRAM-capacity, ir:USER, timing and display-LUT differences. Passing the emulator is not proof.
- The 3D slider can't be changed at runtime: the hotkey and writes to 0x1FF81080 are ignored. Relaunch with `-Factor`.
- Emulator frame timing isn't hardware timing. Use hardware perf logs: the plugin writes `SD:/luma/plugins/<tid>/t3d_log.txt` once per second. Read it first whenever the card comes back.
- Only one agent drives the emulator or desktop at a time.

## Method that worked
- Identify draw passes by giving each pass a unique horizontal shift, then measuring the shifts in a screenshot (`tools/measure_shifts.py`). Name passes from evidence, not guesses (an early guess mislabelled ground vs trees).
- Verify struct fields from the disassembly before using them. A guessed "consumable" offset (0x6A) was wrong; the real one is +0x75 bit 0x20.
- Version-check the exact title and update (full .text CRC) and refuse to patch anything else.
- Check original instruction words before hooking (`Install(addr, fn, expectedWord)`).
- Log context alongside FPS (3D on or off, slider, preset, zoom). In the v0.4.3 stress test, top-screen render CPU stayed ~8.5 ms while fps fell to the 30s, which proved the bottleneck was elsewhere (game logic or GPU), not the stereo.
- Make visual mods reversible at runtime: the UI theme rewrites texels only if they still hold the expected value, so Stock restores them exactly.

## Tooling mistakes
- A script named `dis.py` shadowed Python's stdlib `dis`, breaking capstone. Don't name scratch files after stdlib modules, and run Python with `-I`.
- Heredoc/Python edits turned `"\\n"` into real newlines in C++ strings. Check string literals after scripted edits.
- In the Azahar key map, "cright" is the circle pad, not the C-stick. The C-stick needed explicit scan codes.
- Windows SD: after a Shell eject the reader shows size 0 until the card is reseated.

## Workflow rules from the user
- Patch-only: never distribute game files. Never use the leaked 4J source. Never touch console system files.
- The user tests on hardware (New 3DS XL, plus a Japanese Old 3DS running US games via Luma region-free and locale emulation). Make defaults sane, and keep every option reversible.
- Sharing is GitHub and YouTube only.

## Added from the Minecraft 3D mod (2026-10-28)
- **VRAM-A is a budget, not free space.** Minecraft 1.9.19 leaves about 9 KiB of VRAM-A spare. While loading, it allocates a 2.625 MiB texture area (0x4F0284) and never checks the result. Our 911 KiB right-eye buffers in VRAM-A made that allocation return 0. The game then DMA'd to address 0 and hung on "Loading" at every slider value. The emulator reproduced this. Map the game's VRAM use before taking any; put right-eye buffers in VRAM-B, below 0x1F5F8000, because nngx uses the last 32 KiB itself. Check free space first, and leave stereo off rather than allocate into the game's space.
- **Dead features get deleted, not just disabled, across updates.** Minecraft v1.0 has a complete stereo renderer behind a kill-switch byte. The 1.9.19 update removed it entirely. Always RE the exact version that runs on the console.
- **Matt records videos on this PC.** Emulator automation that activates windows or sends keybd_event input ruins his takes. Set Azahar's [Audio] volume to 0 before every launch. Prefer focus-free input (minecraft-3ds `tools/azahar_bgkeys.ps1`, which PostMessages the window and starts it minimized without activating it). Stop all desktop driving the moment he says he's recording.
- **A CTRPF hook on the GSP event thread runs on a ~4 KiB stack.** Format OSD strings on the plugin thread; only draw them there.
- **CTRPF breaks the game's own C-stick on New 3DS (Minecraft v0.1.0 on hardware).** By default, CTRPF calls libctru `hidInit()` before the game starts, and on New 3DS that runs `irrstInit()` (IRRST Initialize). Games that read the C-stick through ir:USER (SDK ExtraPad / Circle Pad Pro path: Minecraft, very likely Terraria too, see #6) then get 0,0. Azahar doesn't show it. Fix: in the plugin, `extern "C" bool hidShouldUseIrrst(void){ return false; }` (overrides libctru's weak symbol). Never call irrstInit in a plugin unless the game itself uses ir:rst.

## Test conditions
- **Cheats change performance.** Terraria's Fullbright cheat lights every tile, so the game draws every cave backdrop cell it normally skips in darkness. With it on, caves locked to 29.9 fps; with it off they ran 55-60 (v0.5.1 test, 2026-10-28). Torch-heavy areas are the real worst case (29-39). Always log which cheats are on, and benchmark with gameplay cheats off.
- **The display DMA size follows the LEFT framebuffer only (Minecraft v0.2.0).** The SDK's `nngxSwapBuffersByAddress` sets the LCD format register's DMA-size bits (8-9) from the left address alone: FCRAM gives 0x100 (8-word bursts), VRAM gives 0x300. FCRAM can't do size 3 and goes black, which probably explains #3: Terraria had a VRAM left eye and an FCRAM right eye. A VRAM right eye behind an FCRAM left eye ran with small bursts from the busy VRAM bank. Minecraft v0.1.1 showed "glitchy lines" on hardware that moved with bottom-screen activity, and Azahar showed nothing. **Keep both eyes in the same memory type.** Best is VRAM for both, as Terraria's working setup has it. (Fix still pending hardware confirmation.)
- **Don't assume the game's triple buffer is round-robin.** Minecraft 1.9.19 picks the first free display index (0, else 1, else 2), so it usually alternates 0/1. Any scheme that keeps an older image on screen, such as alternate-eye stereo, needs its own buffer rotation.
- **Measure the GPU before you design around "GPU slack".** Inferring CPU-bound from short present() waits was wrong in-world. Timing RunCmdlist → cmdlist-done showed the GPU at 20 ms of a 24 ms frame. Log GPU time from the very first build.
- **Alternate-eye stereo needs turn compensation.** If the eyes are rendered one frame apart, a camera turn adds false disparity of about yaw-rate × frame time. In Azahar that flipped the horizon in front of the screen. Shifting the older eye by the projected camera change fixes the world. The side effect is that HUD/hand drawn into that image wobble during turns.
- **A projection-shear stereo has no built-in limit on how far things come out of the screen.** Disparity is s·px·(1 − C/D), which runs to minus infinity as D gets small: −56 px against a wall with C = 1.5, −148 px with C = 4. Choose the zero-parallax distance from the nearest the camera can ever get to a surface: C = Dmin·(1 + cap/px). Also edit only the top screen's matrices, because the bottom screen goes through the same uniform path (in Minecraft, an aspect test |m5/m0| ≈ 400/240 does this).
