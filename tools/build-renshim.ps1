# Builds the pass-through d3d.ren shim - Phase 0 of "own the renderer".
#
# 32-bit, because the game is. Output goes to game\d3dshim.ren, beside the real
# d3d.ren, which is NOT touched: the engine picks its renderer from the RenderDll
# console variable, so this is selected with +RenderDll d3dshim.ren and reverted
# by not passing it.

$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Src  = Join-Path $Root 'host\renshim'
$Out  = Join-Path $Root 'game\d3dshim.ren'
$Obj  = Join-Path $Root 'build\renshim'

New-Item -ItemType Directory -Force $Obj | Out-Null
if (Test-Path $Out) { Remove-Item $Out -Force }

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat'
if (-not (Test-Path $vcvars)) {
    $vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path $vcvars)) { throw 'No 32-bit VC environment found.' }
    $vcvars = "`"$vcvars`" x86"
} else {
    $vcvars = "`"$vcvars`""
}

$cl = @(
    '/nologo', '/std:c++17', '/EHsc', '/O2', '/W3', '/MT',
    '/DWIN32', '/D_WINDOWS', '/D_USRDLL', '/DNOMINMAX',
    "/Fo:$Obj\",
    "`"$(Join-Path $Src 'dllmain.cpp')`"",
    '/link', '/DLL', "/DEF:`"$(Join-Path $Src 'd3dshim.def')`"",
    "/OUT:`"$Out`"",
    'kernel32.lib', 'user32.lib'
) -join ' '

Write-Host 'Compiling the d3d.ren shim (x86)...' -ForegroundColor Cyan
$output = cmd /c "$vcvars >nul 2>&1 && cl $cl 2>&1"
$clExit = $LASTEXITCODE
$output | ForEach-Object { if ($_ -match 'error|fatal|warning C4') { Write-Host $_ -ForegroundColor Yellow } }

if ($clExit -ne 0 -or -not (Test-Path $Out)) {
    $output | ForEach-Object { Write-Host $_ }
    throw "shim build failed (cl exit $clExit)."
}

# It must export exactly the three names the engine looks for, undecorated.
$vc64 = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$exports = cmd /c "`"$vc64`" >nul 2>&1 && dumpbin /exports `"$Out`"" 2>&1
$names = $exports | Select-String -Pattern '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\w+)' |
         ForEach-Object { $_.Matches[0].Groups[1].Value }

"{0}  ->  {1:N0} bytes" -f (Split-Path $Out -Leaf), (Get-Item $Out).Length | Write-Host
Write-Host "exports: $($names -join ', ')" -ForegroundColor Cyan

foreach ($need in 'RenderDLLSetup', 'GetSupportedModes', 'FreeModeList') {
    if ($names -notcontains $need) { throw "shim is missing the export $need" }
}
Write-Host 'All three exports present. Shim build complete.' -ForegroundColor Green
