# Builds the Nintendo Switch NRO: out\switch\guwange.nro (devkitA64 GCC through tools\switch\cmake\switch-devkitA64.cmake,
# libnx, Renut's renderer on deko3d; no Vulkan driver is linked). The generated game code must exist (tools\codegen.ps1
# on the PC: a cross build cannot run the code generator), and so must the PC tree's shader cache
# (tools/renut_switch_shader_cache.py, tools/renut_deko3d_shaders.sh).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\build_switch.ps1 [-Jobs 3] [-Clean] [-BuildDir out\switch]
#       [-Lto on|off] [-Pgo none|generate|use] [-ReleaseShaders on|off]
#
# -Lto: link-time optimisation of the default.xex code and the app (GUWANGE_SWITCH_LTO). Off for test builds (the
# default), on for the final build. Without -Lto the build keeps its setting.
# -Pgo (profile-guided optimisation, futari-nx's): generate = the instrumented NRO that collects counts on the console
# (app/src/guwange_pgo.cpp; build it in its own folder: -BuildDir out\switch_pgo); use = the optimised NRO from the
# counts tools\win\pgo_merge.ps1 merged into pgo\; none = neither. Without -Pgo the build keeps its setting.
# -ReleaseShaders (GUWANGE_RENUT_RELEASE_SHADERS): on = the NRO for distribution, with only the shaders no game file holds
# built in (tools/renut_release_shaders.py first); the web installer makes deko3d_glsl.bin beside it. off = every
# translated shader built in. Without -ReleaseShaders the build keeps its setting.
#
# Overrides: $env:GUWANGE_NVK_SDK (the Mesa Horizon SDK: plume's Vulkan headers), $env:DEVKITPRO (default C:\devkitPro).
param(
    [int]$Jobs = 3,
    [switch]$Clean,
    [string]$BuildDir = 'out\switch',
    [ValidateSet('', 'on', 'off')][string]$Lto = '',
    [ValidateSet('', 'none', 'generate', 'use')][string]$Pgo = '',
    [ValidateSet('', 'on', 'off')][string]$ReleaseShaders = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$vs = if ($env:GUWANGE_VSDIR) { $env:GUWANGE_VSDIR } else { 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools' }
$cm = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
# One CMake + Ninja (Windows paths): never MSYS2's.
$env:PATH = (($env:PATH -split ';') | Where-Object { $_ -and $_ -notmatch 'msys2\\usr\\bin' }) -join ';'
$env:PATH = "$cm\CMake\bin;$cm\Ninja;C:\Python314;$env:PATH"
# An MSYS-style /opt/devkitpro from the shell is no path for the Windows tools.
if (-not $env:DEVKITPRO -or $env:DEVKITPRO.StartsWith('/')) { $env:DEVKITPRO = 'C:/devkitPro' }
# The LTO link's temporary files on this drive (C: fills up during LTO).
$tmp = Join-Path $root 'out\tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$env:TMP = $tmp
$env:TEMP = $tmp
$nvk = if ($env:GUWANGE_NVK_SDK) { $env:GUWANGE_NVK_SDK } else {
    'C:/devkitPro/mesa-switch-main-nfsmw-source/mesa-switch-main/mesa-unified-install/opt/devkitpro/portlibs/switch' }
if (-not (Test-Path (Join-Path $nvk 'include\vulkan'))) { throw "no include\vulkan under $nvk (set GUWANGE_NVK_SDK)" }
$build = Join-Path $root $BuildDir
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force (Join-Path $root 'out') | Out-Null
$log = Join-Path $root ('out\' + (Split-Path $BuildDir -Leaf) + '_build.log')
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    $configure = @('-S', "$root\app", '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_TOOLCHAIN_FILE=$root\tools\switch\cmake\switch-devkitA64.cmake",
        "-DREXSDK_DIR=$root\sdk", "-DREXGLUE_SWITCH_NVK_SDK=$nvk",
        '-DPYTHON_EXECUTABLE=C:/Python314/python.exe', '-DPython3_EXECUTABLE=C:/Python314/python.exe')
    # Through cmd like the build below: with $ErrorActionPreference = 'Stop', a CMake warning on stderr would stop it.
    $quoted = ($configure | ForEach-Object { "`"$_`"" }) -join ' '
    & $env:ComSpec /c "cmake $quoted > `"$(Join-Path $root 'out\switch_configure.log')`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $root 'out\switch_configure.log') -Tail 30; throw 'configure failed' }
}
if ($Lto) {
    # Through cmd, as the build below: CMake's warnings on stderr must not stop this script ($ErrorActionPreference).
    $ltoValue = if ($Lto -eq 'on') { 'ON' } else { 'OFF' }
    & $env:ComSpec /c "cmake `"$build`" -DGUWANGE_SWITCH_LTO=$ltoValue > `"$(Join-Path $root 'out\switch_configure.log')`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $root 'out\switch_configure.log') -Tail 30; throw 'configure failed' }
}
if ($Pgo) {
    $pgoValue = if ($Pgo -eq 'none') { '' } else { $Pgo }
    & $env:ComSpec /c "cmake `"$build`" -DGUWANGE_PGO=$pgoValue > `"$(Join-Path $root 'out\switch_configure.log')`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $root 'out\switch_configure.log') -Tail 30; throw 'configure failed' }
}
if ($ReleaseShaders) {
    $rsValue = if ($ReleaseShaders -eq 'on') { 'ON' } else { 'OFF' }
    & $env:ComSpec /c "cmake `"$build`" -DGUWANGE_RENUT_RELEASE_SHADERS=$rsValue > `"$(Join-Path $root 'out\switch_configure.log')`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $root 'out\switch_configure.log') -Tail 30; throw 'configure failed' }
}
& $env:ComSpec /c "cmake --build `"$build`" -j $Jobs > `"$log`" 2>&1"
$code = $LASTEXITCODE
Get-Content $log -Tail 25
if ($code -ne 0) { throw "build failed (exit $code), see $log" }
Get-ChildItem $build -Filter *.nro | Select-Object FullName, Length
