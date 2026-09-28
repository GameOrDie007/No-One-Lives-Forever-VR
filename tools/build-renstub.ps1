# Builds d3dstub.ren - the first renderer we own, and the Phase 1 gate.
#
# 32-bit, because the game is. Output goes to game\d3dstub.ren beside the real
# d3d.ren, which is NOT touched: the engine picks its renderer from the RenderDll
# console variable, so this is selected with +RenderDll d3dstub.ren and reverted
# by not passing it.

$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Src  = Join-Path $Root 'host\renstub'
$Out  = Join-Path $Root 'game\d3dstub.ren'
$Obj  = Join-Path $Root 'build\renstub'

# VRShared.h is the contract between the host and the client, and the
# renderer is now a third party to it. One copy, included from where it
# lives - the host build does exactly the same.
$Shell = Join-Path $Root 'src\nolf1-modernizer\NOLF\ClientShellDLL'
if (-not (Test-Path (Join-Path $Shell 'VRShared.h'))) {
    throw "VRShared.h not found under $Shell - the client tree is missing."
}

New-Item -ItemType Directory -Force $Obj | Out-Null
if (Test-Path $Out) { Remove-Item $Out -Force }

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat'
if (-not (Test-Path $vcvars)) { throw 'No 32-bit VC environment found.' }

# The version resource (see host\res\version.h), linked in with the sources.
. (Join-Path $PSScriptRoot 'version-res.ps1')
$ver = New-VersionResCommand -ObjDir $Obj -FileName 'd3dstub.ren' -Description 'No One Lives Forever VR - Direct3D 11 renderer' -Type dll

$cl = @(
    '/nologo', '/std:c++17', '/EHsc', '/O2', '/W3', '/MT',
    '/DWIN32', '/D_WINDOWS', '/D_USRDLL', '/DNOMINMAX',
    "`"$($ver.Res)`"",
    "/I`"$Shell`"",
    "/Fo:$Obj\",
    "`"$(Join-Path $Src 'dllmain.cpp')`"",
    "`"$(Join-Path $Src 'render2d.cpp')`"",
    "`"$(Join-Path $Src 'render3d.cpp')`"",
    "`"$(Join-Path $Src 'lightmap.cpp')`"",
    "`"$(Join-Path $Src 'vrblock.cpp')`"",
    "`"$(Join-Path $Src 'eyeshare.cpp')`"",
    "`"$(Join-Path $Src 'screenlock.cpp')`"",
    "`"$(Join-Path $Src 'rezfs.cpp')`"",
    "`"$(Join-Path $Src 'spranim.cpp')`"",
    "`"$(Join-Path $Src 'dtx.cpp')`"",
    "`"$(Join-Path $Src 'world.cpp')`"",
    "`"$(Join-Path $Src 'butes.cpp')`"",
    '/link', '/DLL', "/DEF:`"$(Join-Path $Src 'd3dstub.def')`"",
    "/OUT:`"$Out`"",
    # A MAP FILE, so a Windows event log "Fault offset" resolves to a function.
    # Two exit faults on 9 September were placed only by parsing the export
    # table; the third (+0x70f23) needed this.
    "/MAP:`"$(Join-Path $Root 'logs\d3dstub.map')`"",
    'kernel32.lib', 'user32.lib', 'd3d11.lib', 'dxgi.lib', 'd3dcompiler.lib'
) -join ' '

Write-Host 'Compiling the renderer stub (x86)...' -ForegroundColor Cyan
$output = cmd /c "`"$vcvars`" >nul 2>&1 && $($ver.Cmd) && cl $cl 2>&1"
$clExit = $LASTEXITCODE
$output | ForEach-Object { if ($_ -match 'error|fatal') { Write-Host $_ -ForegroundColor Red } }

if ($clExit -ne 0 -or -not (Test-Path $Out)) {
    $output | ForEach-Object { Write-Host $_ }
    throw "stub build failed (cl exit $clExit)."
}

# It must export exactly the three names the engine looks for, undecorated.
$vc64 = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$exports = cmd /c "`"$vc64`" >nul 2>&1 && dumpbin /exports `"$Out`"" 2>&1
$names = $exports | Select-String -Pattern '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\w+)' |
         ForEach-Object { $_.Matches[0].Groups[1].Value }

"{0}  ->  {1:N0} bytes" -f (Split-Path $Out -Leaf), (Get-Item $Out).Length | Write-Host
Write-Host "exports: $($names -join ', ')" -ForegroundColor Cyan

foreach ($need in 'RenderDLLSetup', 'GetSupportedModes', 'FreeModeList') {
    if ($names -notcontains $need) { throw "stub is missing the export $need" }
}
Write-Host 'All three exports present. Stub build complete.' -ForegroundColor Green
