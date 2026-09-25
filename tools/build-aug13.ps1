# Builds the 13 August 2026 client and host - the state whose own notes say
# "RESOLVED - the warping is fixed" - so the premise can be TESTED rather than
# reasoned about.
#
# Three sessions, this one included, have tried to find the regression by
# generating candidates and judging them in the headset. That has cost a
# morning. The state that worked is in git; building it costs no headset time
# at all and settles whether it still works today.
#
# Both halves come from the same day, and the host is compiled against the OLD
# client's VRShared.h, so the wire version (9) matches by construction rather
# than by luck. Mixing a version-11 host with a version-9 client would simply
# refuse to attach, which is at least a loud failure - but a subtler mismatch
# would not be, so it is avoided outright.
#
# Nothing is overwritten. The August rez and host land beside today's, and
# tools\play-aug13.ps1 swaps them in for one run and puts today's back.

param([string]$ClientCommit = 'e82a578', [string]$HostCommit = '2bd27cf')

$ErrorActionPreference = 'Stop'

$Root  = Split-Path $PSScriptRoot -Parent
$Work  = Join-Path $Root 'build\aug13-client'
$Out   = Join-Path $Root 'build\aug13'
$Stage = Join-Path $Out 'rez-stage'
$HSrc  = Join-Path $Out 'host'
$Xr    = Join-Path $Root 'libs\openxr'

New-Item -ItemType Directory -Force $Out, $HSrc | Out-Null

if (-not (Test-Path (Join-Path $Work 'NOLF\NOLF.sln'))) {
    throw "worktree missing at $Work - run: git -C $Root\src\nolf1-modernizer worktree add -f $Work $ClientCommit"
}

# --- client ---------------------------------------------------------------
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -version '[16.0,17.0)' -products * -requires Microsoft.Component.MSBuild `
                      -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'VS2019 (v142) MSBuild not found.' }

Write-Host "[1/4] Compiling the August client..." -ForegroundColor Cyan
& $msbuild (Join-Path $Work 'NOLF\NOLF.sln') /t:Build `
    /p:Configuration='Final Release' /p:Platform=x86 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "August client build failed (exit $LASTEXITCODE)." }

Write-Host '[2/4] Packing its rez...' -ForegroundColor Cyan
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Force $Stage | Out-Null
foreach ($a in @('NOLF\ClientRes\CRes.dll', 'NOLF\ClientShellDLL\CShell.dll', 'NOLF\ObjectDLL\Object.lto')) {
    $p = Join-Path $Work $a
    if (-not (Test-Path $p)) { throw "missing August artifact: $a" }
    Copy-Item $p $Stage
}
Copy-Item (Join-Path $Work 'ASSETS\*') $Stage -Recurse

$rez = Join-Path $Out 'Modernizer.rez'
if (Test-Path $rez) { Remove-Item $rez -Force }
& (Join-Path $Work 'TOOLS\lithrez.exe') c $rez $Stage | Out-Null
if (-not (Test-Path $rez)) { throw 'lithrez produced no output for the August build.' }

# --- host -----------------------------------------------------------------
# Extracted from git rather than checked out, so the working tree is untouched.
Write-Host "[3/4] Extracting the August host ($HostCommit)..." -ForegroundColor Cyan
foreach ($f in @('vrmain.cpp', 'xrvr.cpp', 'xrvr.h', 'capture.cpp', 'capture.h',
                 'mirror.cpp', 'mirror.h', 'hostlog.h')) {
    $blob = & git -C $Root show "${HostCommit}:host/$f" 2>$null
    if ($LASTEXITCODE -ne 0) { throw "August host is missing host/$f at $HostCommit" }
    # Write-Output through Set-Content would re-encode; -Encoding ascii keeps
    # these ASCII sources byte-exact and adds no BOM for the compiler to choke on.
    $blob | Set-Content -Path (Join-Path $HSrc $f) -Encoding ascii
}

$Exe = Join-Path $Root 'host\nolfvr-aug13.exe'
if (Test-Path $Exe) { Remove-Item $Exe -Force }

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$sources = @('vrmain.cpp', 'xrvr.cpp', 'capture.cpp', 'mirror.cpp') |
           ForEach-Object { "`"$(Join-Path $HSrc $_)`"" }
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

# The include path points at the AUGUST client's ClientShellDLL, so VRShared.h
# is version 9 on both sides.
$cl = @(
    '/nologo', '/std:c++17', '/EHsc', '/O2', '/W3', '/MD',
    '/DUNICODE', '/D_UNICODE', '/DNOMINMAX',
    "/I`"$Xr\include`"",
    "/I`"$Work\NOLF\ClientShellDLL`"",
    "/Fo:$obj\", "/Fe:$Exe",
    ($sources -join ' '),
    '/link', "`"$Xr\native\x64\release\lib\openxr_loader.lib`"",
    'windowsapp.lib', 'user32.lib', 'shcore.lib'
) -join ' '

Write-Host '[4/4] Compiling the August host (x64)...' -ForegroundColor Cyan
$output = cmd /c "`"$vcvars`" >nul 2>&1 && cl $cl 2>&1"
$clExit = $LASTEXITCODE
$output | ForEach-Object { if ($_ -match 'error|fatal') { Write-Host $_ -ForegroundColor Red } }
if ($clExit -ne 0 -or -not (Test-Path $Exe)) {
    $output | ForEach-Object { Write-Host $_ }
    throw "August host build failed (cl exit $clExit)."
}

Write-Host ''
Write-Host "August rez : $rez  ($('{0:N0}' -f (Get-Item $rez).Length) bytes)" -ForegroundColor Green
Write-Host "August host: $Exe  ($('{0:N0}' -f (Get-Item $Exe).Length) bytes)" -ForegroundColor Green
Write-Host 'Run it with tools\play-aug13.ps1' -ForegroundColor Green
