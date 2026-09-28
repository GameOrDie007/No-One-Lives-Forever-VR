# ---------------------------------------------------------------------------
# canopy-ours.ps1 - OUR renderer, flat on the monitor. No headset, no host.
#
#   powershell -ExecutionPolicy Bypass -File tools\canopy-ours.ps1
#
# The partner to tools\canopy-retail.ps1. Same game, same level, same window
# size - the only difference is that d3dstub.ren draws it instead of Monolith's
# d3d.ren. So anything that differs between the two folders is OUR RENDERER and
# nothing else.
#
# FLAT AND MONO ON PURPOSE, AND THIS IS THE POINT WORTH KNOWING: the canopy bug
# is in the renderer, not in VR. You do not need the headset to reproduce it,
# and taking VR out removes stereo, the compositor, the video encoder and the
# 90 Hz deadline from the comparison. It also means you can do this sitting at
# the desk in two minutes instead of putting the Quest on.
#
# WHAT TO DO - the same as the retail script, in the same place:
#
#   1. Run it. The game opens windowed, straight into Morocco.
#   2. Walk to the canopy.
#   3. LOOK AT IT AND KEEP MOVING A LITTLE. This matters more than anything
#      else here: two coincident surfaces only fight when the camera moves. A
#      frozen view shows a correct picture in both builds - that is exactly how
#      every desk measurement of this bug has been wrong so far.
#   4. Press Enter in THIS window to grab a burst.
#   5. Q then ENTER when done. It scans what it captured and tells you.
# ---------------------------------------------------------------------------
param(
    # See the note in canopy-retail.ps1: nobody has established which Morocco
    # scene the sniper roof is in, and the two candidates fail differently.
    [string]$Scene   = 'M01S01',
    [switch]$Menu,
    [int]   $Frames  = 24,
    [int]   $ResW    = 1280,
    [int]   $ResH    = 692,
    [string]$Out     = 'logs\canopy\ours',
    # THE POSITIVE CONTROL, and the reason to bother with it: this restores the
    # OLD behaviour, where both coincident brushes were drawn. If a run with
    # this on does NOT flicker, the camera was not looking at the canopy and a
    # clean result from the normal arm means nothing. A desk run on 11
    # September returned 0.27% and 0.28% for the two arms and was void for
    # exactly this reason.
    [switch]$DrawHidden,
    # God mode and the full arsenal, so you can get onto a roof without
    # fighting your way there.
    [switch]$God
)

$ErrorActionPreference = 'Stop'
$Root   = Split-Path $PSScriptRoot -Parent
$Game   = Join-Path $Root 'game'
$OutAbs = Join-Path $Root $Out
# NEVER KILL A GAME THIS SCRIPT DID NOT START. A running lithtech.exe is most
# likely a player's session; stop and say so instead of closing it.
if (Get-Process lithtech -ErrorAction SilentlyContinue) {
    Write-Host '  STOPPED: the game is already running (a player may be in it). Close it first; nothing was touched.' -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host '  OURS - d3dstub.ren, the renderer we wrote. Flat and mono.' -ForegroundColor Cyan
Write-Host ''

if (-not (Test-Path (Join-Path $Game 'd3dstub.ren'))) {
    throw "game\d3dstub.ren is missing - run tools\build-renstub.ps1 first."
}

# The settings that take VR out of the picture without taking OUR RENDERER out
# of it. VRStereo=0 gives one view, not a side-by-side pair, so the capture is
# directly comparable to the retail one. StubNativeFrustum=0 because the
# asymmetric per-eye projection has nothing to do with a z-fight and would only
# make the two pictures harder to line up.
$set = @('VRStereo=0', 'StubNativeFrustum=0', 'VRAsymFrustum=0', 'VRCrosshair=0')

if ($DrawHidden) {
    $set += 'StubDrawHiddenWM=1'
    Write-Host '  POSITIVE CONTROL: StubDrawHiddenWM 1 - the OLD behaviour, both' -ForegroundColor Yellow
    Write-Host '  coincident brushes drawn. THIS ARM IS SUPPOSED TO FLICKER.' -ForegroundColor Yellow
    Write-Host '  If it does not, the camera is not on the canopy and neither arm counts.' -ForegroundColor Yellow
}
if ($God) {
    $set += @('VRCheats=7', 'VRDebugGod=1', 'VRDebugArsenal=1', 'VRDebugMissions=1', 'VRDebugClip=1')
    Write-Host '  GOD MODE + every weapon + noclip, so you can reach the roof.' -ForegroundColor Green
} else {
    # Explicitly OFF, never merely omitted: the engine PERSISTS these into
    # autoexec.cfg, so a previous run that set them leaves them on and the next
    # one silently inherits god mode. Omitting a switch does not select its
    # default.
    $set += @('VRCheats=0', 'VRDebugGod=0', 'VRDebugArsenal=0', 'VRDebugMissions=0', 'VRDebugClip=0')
}
if (-not $Menu) {
    $set += ('runworld=Worlds\' + $Scene)
    Write-Host ('  loading straight into {0}' -f $Scene) -ForegroundColor Cyan
    Write-Host '  Not the right roof? Close it and add  -Scene M01S03' -ForegroundColor Yellow
} else {
    Write-Host '  starting at the main menu - load your own save.' -ForegroundColor Cyan
}

# Launched in the background so this window keeps the prompt. run.ps1 blocks
# until the game exits, which is right for a harness and wrong here.
$runner = Start-Process -FilePath 'powershell' -PassThru -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $PSScriptRoot 'run.ps1'),
    '-RenderDll', 'd3dstub.ren',
    '-Title', 'OURS (d3dstub)',
    '-Width', "$ResW", '-Height', "$ResH",
    '-Set', ($set -join ',')
)

# THE GAME THIS SCRIPT STARTED is run.ps1's child, found by parent pid and
# never by name: any other lithtech.exe may be a player's.
function Get-OwnGame($runner) {
    $c = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($runner.Id) AND Name='lithtech.exe'" -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($c) { try { return Get-Process -Id $c.ProcessId -ErrorAction Stop } catch {} }
    return $null
}

Write-Host '  starting...' -ForegroundColor DarkGray
$deadline = (Get-Date).AddSeconds(40)
$g = $null
while (-not $g -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $g = Get-OwnGame $runner
}
if (-not $g) {
    throw 'the game did not start within 40 s - see the console window it opened.'
}
Start-Sleep -Seconds 4

Write-Host ''
Write-Host '  ============================================================' -ForegroundColor Cyan
Write-Host '   Walk to the canopy. Look at it, and KEEP THE VIEW MOVING.' -ForegroundColor Cyan
Write-Host '   A still camera hides this bug completely.' -ForegroundColor Cyan
Write-Host '   Then come back here and press ENTER to grab a burst.' -ForegroundColor Cyan
Write-Host '   Q then ENTER when you are done.' -ForegroundColor Cyan
Write-Host '  ============================================================' -ForegroundColor Cyan
Write-Host ''

New-Item -ItemType Directory -Force $OutAbs | Out-Null
$burst = 0
$dirs  = @()
while ($true) {
    if ($g.HasExited) {
        Write-Host '  the game closed.' -ForegroundColor Yellow; break
    }
    $k = Read-Host 'ENTER to capture, Q to finish'
    if ($k -match '^\s*[qQ]') { break }
    if ($g.HasExited) {
        Write-Host '  the game closed.' -ForegroundColor Yellow; break
    }
    $burst++
    $d = Join-Path $OutAbs ("burst{0}" -f $burst)
    Write-Host ("  burst {0}..." -f $burst) -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'canopy-burst.ps1') `
        -Out $d -Label 'ours' -Count $Frames -Width $ResW -Height $ResH | Out-Host
    $dirs += $d
}

# Only the process this script started.
if (-not $g.HasExited) { try { $g.CloseMainWindow() | Out-Null } catch {} }
Start-Sleep -Seconds 2
if (-not $g.HasExited) { try { $g.Kill() } catch {} }
if ($runner -and -not $runner.HasExited) { try { $runner.Kill() } catch {} }

# ---- read it back -------------------------------------------------------
#
# PNGs, not the renderer's BMP dumps, so the scanner is pointed at these.
Write-Host ''
foreach ($d in $dirs) {
    $n = (Get-ChildItem $d -Filter '*.png' -ErrorAction SilentlyContinue).Count
    Write-Host ("=== {0}  ({1} frames) ===" -f (Split-Path $d -Leaf), $n) -ForegroundColor Magenta
    if ($n -lt 6) { Write-Host '   too few frames to say anything' -ForegroundColor Yellow; continue }
    & python (Join-Path $PSScriptRoot 'flicker-scan.py') $d `
        --out (Join-Path $d 'flicker.png') 2>&1 | ForEach-Object { Write-Host ('   ' + $_) }
}

Write-Host ''
Write-Host ('  saved under {0}' -f $OutAbs) -ForegroundColor Green
Write-Host '  HOW TO READ IT' -ForegroundColor Cyan
Write-Host '   A percent-scale alternating fraction landing in one patch is the'
Write-Host '   canopy fighting. A few tenths of a percent scattered about is nothing.'
Write-Host '   And a clean result only counts if a -DrawHidden run of the SAME spot'
Write-Host '   came back dirty - otherwise the camera was not on the canopy.'
Write-Host ''
