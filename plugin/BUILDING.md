# Building Terraria3D.3gx

Requirements (kept outside this repo):

- devkitPro with devkitARM (Windows: run via its msys2 shell; `DEVKITPRO`/`DEVKITARM` set).
- CTRPluginFramework (libctrpf) and 3gxtool 1.3, placed in a `toolchain/` folder
  two levels above `plugin/`:

  ```
  <projects>/toolchain/libctrpf/
  <projects>/toolchain/3gxtool-1.3/3gxtool.exe
  <projects>/<repo>/plugin/
  ```

  Or override on the command line: `make TOOLCHAIN=/path/to/toolchain`
  (or `CTRPFLIB=` / `GXTOOL=`).

Build:

- Windows: `./build.ps1` (calls `build.sh` inside the devkitPro msys2 shell;
  set `DEVKITPRO_WIN` if devkitPro is not in `C:\devkitPro`).
- msys2/Linux: `./build.sh` (release), `./build.sh debug`, `./build.sh clean`.

devkitPro's make rules break on paths containing spaces, so `build.sh` builds through
a space-free directory junction. Output: `Terraria3D.3gx`, which goes to
`SD:/luma/plugins/000400000016A900/`.

Old 3DS build: `./build.ps1 o3ds` (or `./build.sh o3ds`). It compiles the same source
with `-DT3D_O3DS=1` and `Terraria3D-o3ds.plgInfo` (2 MiB plugin memory) into
`Terraria3D-o3ds.3gx`. Rename it to `Terraria3D.3gx` on the SD card.
