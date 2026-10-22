# Building guwange-nx

How to build the Windows PC build (used for development and testing), the Nintendo Switch NRO and its shaders from
your own copy of the game. To just play it, use the installer page instead (README). The tree is futari-nx's, so
[futari-notes.md](futari-notes.md) explains most of the tools named here.

## What you need

- **Your own copy of Guwange for Xbox Live Arcade** (title `58410A78`): the game's content package (content type
  `000D0000`) and its Title Update #1 package (content type `000B0000`, `TU_1C422JO_000000G000000.0000000000084`).
- **Windows 10/11** with:
  - VS 2022 Build Tools (the MSVC libraries and the Windows SDK; CMake and Ninja come with it);
  - LLVM clang 20 (a portable copy is fine);
  - Python 3 and Git.

  `tools/win/env.ps1` finds them.
- **For the Switch:** [devkitPro](https://devkitpro.org) with devkitA64, libnx and deko3d, and the Vulkan headers of
  nfsmw-nx's Mesa build (`GUWANGE_NVK_SDK`; plume is built with them, the NRO links no Vulkan driver).

Run every build command from PowerShell, through `tools\win\run.ps1` or the scripts below (they load
`tools\win\env.ps1`). Running `cmake` from a Bash shell picks up MSYS2's tools: a configure started there fails and
wipes the build folder's cache.

## 1. Extract the game

The game and its title update go into one folder, `assets\game_root` (not committed): the update's `default.xexp`
beside `default.xex` and its `.dllp` files beside the cores in `module\`. The runtime and the code generator apply a
sibling `p` patch by themselves.

```powershell
python tools\fetch_thirdparty.py                         # third-party sources of the SDK (ReXGlue SDK v0.10.0)
python tools\stfs_extract.py <game package> assets\game_root
python tools\stfs_extract.py <title update package> assets\game_root
```

## 2. Translate the game's code to C++

```powershell
powershell -File tools\win\run.ps1 out\host_configure.log "cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2"
powershell -File tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue"
powershell -File tools\codegen.ps1                       # default.xex and the four cores -> app\generated (git-ignored)
python tools\renut_guwange_hooks.py                      # the renderer's hook sites in the generated code
```

`tools\codegen.ps1` reads `app\guwange_manifest.toml` (the generator's options are in the files it includes:
`app\guwange_overrides*.toml`, `app\guwange_perf.toml`) and writes the decompressed images of the five executables
(`out\*_image.bin`), which the shader tools read. After the generation it runs `tools\guwange_post_codegen.py`:
`tools\spin_forever.py` (a correctness step: guest loops that never end, which clang would otherwise delete) and
futari-nx's performance steps (`tools\post_codegen.py`). The build runs them again after every code generation, and a
second run changes nothing. `-NoPerf` runs the correctness step only; every performance step has a CMake switch back
(`GUWANGE_GEN_*`, `GUWANGE_SWITCH_*` in `app/CMakeLists.txt`). `-Force` regenerates after a change of the generator
itself.

## 3. The PC build

```powershell
powershell -File tools\win\run.ps1 out\pc_configure.log "cmake --preset win-pc -S app"
powershell -File tools\win\run.ps1 out\pc_build.log "cmake --build out/pc -j 4"
powershell -File tools\win\boot_test.ps1                 # muted and minimized, then the end of the log
```

`out\pc\guwange.toml` holds the PC's settings; test runs are muted and minimized. `-VideoAt <seconds>` and
`-VideoFrames` record the game image to `out\pc\captures\` (`tools/make_video.sh` makes the MP4), and `-Pad` scripts
the controller (`app/src/guwange_input_script.h`).

## 4. The shaders

The deko3d renderer draws with the game's shaders translated ahead of time. Game data: `out/` only.

```bash
python tools/renut_dump_shaders.py      # the shader containers of the five images -> out/renut_shader_dump
# the PC build (section 3) translates them with Renut's XenosRecomp and its early alpha copies
bash tools/renut_deko3d_shaders.sh      # SPIR-V -> GLSL -> banjo-nx's passes -> every shader in the NRO (development)
python tools/renut_release_shaders.py   # the release build's files (out/renut_switch_release)
```

The released NRO carries only the 9 shaders that no game file holds (`BUILT_IN` in `tools/renut_release_shaders.py`);
the other 78 come from `deko3d_glsl.bin` beside it, which the installer page makes from the player's files
([guwange-nx-installer](https://github.com/ChanseyIsTheBest/guwange-nx-installer): the 68 containers of `default.xex`
with the title update applied, and the 10 of the sprite effects in `res/DSD000.bin` and `DSD100.bin`, decoded with the
game's own LZSS). For a test of a release build, put the installer's file (or
`out/renut_switch_release/sd/deko3d_glsl.bin`, the Python tools' copy) next to the NRO.

## 5. The Switch NRO

```powershell
powershell -File tools\win\build_switch.ps1 -Jobs 3 [-Lto on|off] [-Pgo none|generate|use] [-ReleaseShaders on|off]
python tools\check_switch_build.py                       # start-up code at 0, no vector FMA, no self-indexed table
                                                         # lookup, no fcvtas in game code
python tools\find_self_loops.py out\switch\guwange       # no branch-to-self loops in game code
python tools\plume_vma_names.py --check out\switch\guwange
```

`build_switch.ps1` configures `out\switch` the first time (`-BuildDir` builds elsewhere); each option keeps its last
setting when left out. The release is `-Lto on -Pgo use -ReleaseShaders on`; test builds are faster without LTO. The
NRO's name, author, version and icon (`app/guwange_icon.jpg`) are in `app/CMakeLists.txt`. The settings compiled into
the NRO are `app/guwange_switch_defaults.toml` (written by `tools/make_guwange_configs.py` from the enhancement table of
`app/src/guwange_enhancements.cpp`), and the player's `guwange.toml` is `app/guwange_switch.toml`. Set `TMP` and `TEMP`
to a folder with several GB free for the LTO link (`build_switch.ps1` uses `out\tmp`).

### Profile-guided optimisation

`pgo/` holds the profile the released NRO was built with (one console session). `-Pgo use` builds with it; a function
you change loses its part of it, and the rest still applies. The files are named by the object's path in the build
folder, so build in `out\switch` for them to apply. To record a new profile:

1. `build_switch.ps1 -Pgo generate -BuildDir out\switch_pgo` builds an instrumented NRO. Play it on the console: it
   writes `pgo/guwange_pgo_<N>.stream` next to the NRO (one file per session).
2. Move the `.gcda` files of `pgo\` aside, copy the console's streams to a folder on the PC, and run
   `powershell -File tools\win\pgo_merge.ps1 <folder>`. Merge on the PC that built the instrumented NRO.
3. `build_switch.ps1 -Pgo use` builds the optimised NRO.

## Running in Ryujinx

Put the NRO, `deko3d_glsl.bin` (for a release build), `guwange.toml` and `game_root` in `sdmc:/switch/guwange-nx` of
the emulator (the SDK falls back to that folder when the emulator gives no argv).
