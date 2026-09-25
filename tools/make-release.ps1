# ---------------------------------------------------------------------------
# make-release.ps1 - build the archive somebody else can actually install.
#
# The whole risk of shipping a game mod sits in two places: what ends up in the
# archive, and whether the thing tested is the thing shipped. This addresses
# both and refuses to finish if it cannot.
#
# WHAT MAY SHIP IS AN ALLOW-LIST, not a list of things to exclude. A block-list
# is only as good as the last time somebody remembered to extend it; an
# allow-list fails closed on anything new. NOLF's own data - the .rez archives,
# the levels, the art, the sound - is never ours to give away, and the staging
# folder this project develops in is full of it.
#
#   .\tools\make-release.ps1                 -> dist\No-One-Lives-Forever-VR-<date>.zip
#   .\tools\make-release.ps1 -Version 0.2
# ---------------------------------------------------------------------------
param(
    [string]$Version = '',
    [string]$OutDir  = 'dist',
    [switch]$SkipBuild,
    # Package anyway with the debug tools compiled in. For a build going to
    # one tester who needs the switches, never for a public release.
    [switch]$AllowDebugTools
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'

if (-not $Version) { $Version = (Get-Date -Format 'yyyy-MM-dd') }
$Name = "No-One-Lives-Forever-VR-$Version"

# A HALF-FINISHED INSTALL IS WORSE THAN A STALE ONE. The build writes into
# game\, and a running game holds a lock on part of it.
$running = Get-Process lithtech -ErrorAction SilentlyContinue
if ($running) {
    throw "lithtech.exe is running (pid $($running.Id)). Close the game first - nothing was built."
}

# ---- THE DEBUG TOOLS MUST BE COMPILED OUT --------------------------------
#
# VR_DEBUG_TOOLS gates a set of switches that are cheats in a player's hands -
# VRDebugGod, VRDebugClip, VRDebugNoAI, VRDebugArsenal - plus the desk
# instruments that end cinematics and hold the trigger. They are all off by
# default, and "off by default" is exactly the guarantee that has failed this
# project before: a setting the program persists is never at its default on
# the machine that has been testing it.
#
# Checked against the SOURCE rather than the binary, which is a proxy and is
# stated as one: it establishes what the next build will contain, and since
# the build above is not skipped by default, what this archive contains.
$vrdt = Join-Path $Root 'src\nolf1-modernizer\NOLF\ClientShellDLL\GameClientShell.cpp'
if (Test-Path -LiteralPath $vrdt) {
    $line = Select-String -LiteralPath $vrdt -Pattern '^#define\s+VR_DEBUG_TOOLS\s+(\d+)' | Select-Object -First 1
    if ($line -and $line.Matches[0].Groups[1].Value -ne '0') {
        if ($AllowDebugTools) {
            Write-Host ''
            Write-Host 'WARNING: VR_DEBUG_TOOLS is 1 - this archive carries the debug' -ForegroundColor Yellow
            Write-Host 'switches and the cheats. -AllowDebugTools was given, so continuing.' -ForegroundColor Yellow
            Write-Host ''
        } else {
            throw ("REFUSING TO PACKAGE: VR_DEBUG_TOOLS is " +
                   $line.Matches[0].Groups[1].Value + " in GameClientShell.cpp, so this " +
                   "build would ship the debug switches and the cheats. Set it to 0 and " +
                   "rebuild, or pass -AllowDebugTools for a tester-only build. Nothing was written.")
        }
    }
}

if (-not $SkipBuild) {
    Write-Host '=== building the client and the renderer ===' -ForegroundColor Cyan
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1') | Select-Object -Last 2
    if ($LASTEXITCODE) { throw "the client build failed (exit $LASTEXITCODE) - nothing was packaged." }
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-renstub.ps1') | Select-Object -Last 2
    if ($LASTEXITCODE) { throw "the renderer build failed (exit $LASTEXITCODE) - nothing was packaged." }
}

# ---- THE ALLOW-LIST ------------------------------------------------------
#
# source (relative to the repo) -> where it goes in the archive.
# Everything else is refused, by construction: nothing is copied that is not
# named here.
$Ship = @(
    @{ from = 'game\Modernizer.rez';    to = 'game\Modernizer.rez';     what = 'the VR game client, packed' },
    @{ from = 'game\d3dstub.ren';       to = 'game\d3dstub.ren';        what = 'the D3D11 renderer' },
    @{ from = 'host\nolfvr.exe';        to = 'host\nolfvr.exe';         what = 'the OpenXR host' },
    @{ from = 'host\openxr_loader.dll'; to = 'host\openxr_loader.dll';  what = 'the OpenXR loader' },
    @{ from = 'config\vrtune.cfg';     to = 'game\vrtune.cfg';         what = 'the tuned muzzle points, every gun, tuned in headset testing' },
    @{ from = 'config\vrweapons.cfg';   to = 'game\vrweapons.cfg';       what = 'per-weapon view-model scale overrides' },
    @{ from = 'game\SDL2.dll';         to = 'game\SDL2.dll';           what = 'SDL2 - the client DLL imports it and retail has none' },
    @{ from = 'game\DINPUT.dll';       to = 'game\DINPUT.dll';         what = 'the keyboard proxy - chat and console keys in VR' },
    @{ from = 'tools\play-vr.ps1';      to = 'tools\play-vr.ps1';       what = 'the launcher' },
    @{ from = 'tools\run.ps1';          to = 'tools\run.ps1';           what = 'the launcher''s second half - play-vr.ps1 calls it; the 21 Sep install test shipped without it and could not launch' },
    @{ from = 'tools\repair-settings.ps1'; to = 'tools\repair-settings.ps1'; what = 'the settings repair' },
    @{ from = 'tools\setup.ps1';        to = 'tools\setup.ps1';        what = 'setup - finds the game (folder, discs, zip) and builds game\ beside it' },
    @{ from = 'tools\collect-report.ps1'; to = 'tools\collect-report.ps1'; what = 'zips the latest logs onto the Desktop for a bug report' },
    @{ from = 'README.md';              to = 'README.md';               what = 'the readme' },
    @{ from = 'THIRD-PARTY-NOTICES.md'; to = 'THIRD-PARTY-NOTICES.md';  what = 'SDL2 and OpenXR loader licenses, as they require' }
    # (docs\MOTION-CONTROLS.md no longer ships: it is a development note from
    # before most of the controls existed, and the README carries the controls.)
)

# THE FILES A USER DOUBLE-CLICKS. Written here rather than kept in the tree, so
# the archive can never carry a stale copy of them. -ExecutionPolicy Bypass
# applies to that one run and changes no system setting. %* carries whatever
# was dragged onto Setup.bat (a game folder, both disc images, a zip).
$SetupBat = @'
@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
pause
'@
$PlayBat = @'
@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\play-vr.ps1" -Native %*
if errorlevel 1 pause
'@
$ReportBat = @'
@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\collect-report.ps1" %*
pause
'@

# ---- stage ---------------------------------------------------------------
$Stage = Join-Path $env:TEMP ("nolfvr-stage-" + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Force $Stage | Out-Null
$missing = @()
foreach ($f in $Ship) {
    $src = Join-Path $Root $f.from
    if (-not (Test-Path -LiteralPath $src)) { $missing += $f.from; continue }
    $dst = Join-Path $Stage $f.to
    [System.IO.Directory]::CreateDirectory((Split-Path $dst -Parent)) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $len = (Get-Item -LiteralPath $dst).Length
    Write-Host ("  {0,-34} {1,10:N0} bytes   {2}" -f $f.to, $len, $f.what)
}
if ($missing.Count) {
    Remove-Item -Recurse -Force $Stage
    throw ("not built yet: " + ($missing -join ', ') + " - nothing was packaged.")
}
Set-Content -LiteralPath (Join-Path $Stage 'Setup.bat') -Value $SetupBat -Encoding ASCII
Set-Content -LiteralPath (Join-Path $Stage 'Play NOLF VR.bat') -Value $PlayBat -Encoding ASCII
Set-Content -LiteralPath (Join-Path $Stage 'Collect report.bat') -Value $ReportBat -Encoding ASCII
Set-Content -LiteralPath (Join-Path $Stage 'version.txt') -Value ("No One Lives Forever VR " + $Version) -Encoding ASCII

# ---- the refusal ---------------------------------------------------------
#
# Belt and braces over the allow-list: walk what is actually staged and refuse
# on anything that looks like the game's own data, whatever put it there.
$banned = Get-ChildItem -LiteralPath $Stage -Recurse -File | Where-Object {
    $_.Extension -match '^\.(rez|dat|dtx|abc|wav|sav|pcx|spr|ltb)$' -and $_.Name -ne 'Modernizer.rez'
}
if ($banned) {
    $names = ($banned | ForEach-Object { $_.Name }) -join ', '
    Remove-Item -Recurse -Force $Stage
    throw "REFUSING TO PACKAGE: game data in the archive ($names). Nothing was written."
}
# Modernizer.rez is ours - it holds the client we compiled - but prove it is
# not a copy of one of the game's archives that happens to share the name.
$mod = Get-Item -LiteralPath (Join-Path $Stage 'game\Modernizer.rez')
foreach ($g in @('NOLF.rez','NOLF2.rez','NolfGoty.rez')) {
    $p = Join-Path $Game $g
    if ((Test-Path -LiteralPath $p) -and (Get-Item -LiteralPath $p).Length -eq $mod.Length) {
        Remove-Item -Recurse -Force $Stage
        throw "REFUSING TO PACKAGE: game\Modernizer.rez is the same size as $g. Nothing was written."
    }
}

# ---- archive -------------------------------------------------------------
$Out = Join-Path $Root $OutDir
New-Item -ItemType Directory -Force $Out | Out-Null
$Zip = Join-Path $Out ($Name + '.zip')
# Not Compress-Archive: it writes backslash entry names (see write-zip.ps1).
. (Join-Path $PSScriptRoot 'write-zip.ps1')
Write-ReleaseZip -Folder $Stage -Zip $Zip

# WHAT WAS SHIPPED, so a bug report can be matched to a build.
Write-Host ''
Write-Host ("archive: {0}  ({1:N1} MB)" -f $Zip, ((Get-Item $Zip).Length / 1MB)) -ForegroundColor Green
Write-Host 'contents:'
Get-ChildItem -LiteralPath $Stage -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($Stage.Length + 1)
    $h = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.Substring(0, 16)
    Write-Host ("  {0,-34} {1}" -f $rel, $h)
}
Remove-Item -Recurse -Force $Stage
Write-Host ''
Write-Host 'Not yet tested. Extract it somewhere with a space in the path and run Setup.bat.' -ForegroundColor Yellow
exit 0
