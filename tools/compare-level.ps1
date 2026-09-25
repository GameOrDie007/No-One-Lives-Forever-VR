# The retail renderer and ours, on the SAME level, from the SAME viewpoint,
# with nobody in the room.
#
# This is a tooling gap that stood open for weeks. Every
# judgement about "what is missing" has been read off a phone photograph of a
# headset, because the two renderers could only be compared by loading a quick
# save - which spawns looking at the floor - and the desk harness cannot turn
# the camera.
#
# `+runworld <world>` removes the problem instead of solving it. It starts the
# level at its OWN designated start point, facing the direction the level
# designer chose, and it does that identically for both renderers because it
# happens before either of them is asked to draw anything. So the two pictures
# are of the same view by construction, not by a person standing still.
#
#   .\tools\compare-level.ps1 -World Worlds\T01S02
#   .\tools\compare-level.ps1 -World Worlds\M01S02 -Wait 20
#
# Writes logs\compare\<WORLD>\{retail.png, ours.png, ours-renstub.log} and
# prints the mean colour of each, which is the cheap tell for "one of them
# drew nothing".

param(
    [Parameter(Mandatory=$true)][string]$World,
    [int]$Wait = 14,
    [string]$OutDir = 'logs\compare',
    # THE RESOLUTION, EXPLICIT. Without it both runs take whatever the game's
    # saved config holds, and after a night of sweeps that was 960x720 - the
    # retail capture came back black and ours a quarter size. The sweep and
    # look-shot pass theirs; this now does the same.
    [int]$ResW = 2560,
    [int]$ResH = 1384,
    [string[]]$Set = @()
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'
# THE PLAYER'S SETTINGS ARE NOT THE DESK'S. The engine writes autoexec.cfg
# when the game closes, and this harness closes it gracefully - so a run
# that passed +soundenable 0 left the next headset session with the effects
# volume off and the quality low (9 September). The file is snapshotted
# before the run and put back after; a snapshot left by a killed run is
# restored first, so it can never leak into a headset session (play-vr
# restores it too).
$cfgLive = Join-Path $Game 'autoexec.cfg'
$cfgSnap = Join-Path $Root 'logs\autoexec.headset.cfg'
if (Test-Path $cfgSnap) { Copy-Item $cfgSnap $cfgLive -Force }
elseif (Test-Path $cfgLive) { Copy-Item $cfgLive $cfgSnap -Force }

$tag  = ($World -replace '\.DAT$','') -replace '[/\\]','_'
$Out  = Join-Path $Root (Join-Path $OutDir $tag)
New-Item -ItemType Directory -Force $Out | Out-Null

Add-Type -AssemblyName System.Windows.Forms
$primary = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Primary } | Select-Object -First 1
$rez = @('NOLF.rez','NOLF2.rez','NOLFdll.rez','NOLFl.rez','custom',
         'Nolfu003.rez','Nolfcres003.rez','NolfGoty.rez','Modernizer.rez')

function Run-One([string]$renderer, [string]$title, [string]$png, [string[]]$extra) {
    Get-Process lithtech -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
    Start-Sleep -Milliseconds 600

    $a = @('-windowtitle', $title)
    foreach ($r in $rez) { $a += @('-rez', $r) }
    $a += @('+multiplayer','0')
    if ($primary) { $a += @('+CardDesc', $primary.DeviceName) }
    $a += @('+RenderDll', $renderer)
    $a += @('+ScreenWidth', "$ResW", '+ScreenHeight', "$ResH")
    foreach ($e in $extra) { $n,$v = $e -split '=',2; $a += @("+$n", $v) }
    foreach ($e in $Set)   { $n,$v = $e -split '=',2; $a += @("+$n", $v) }
    # Quoted: three worlds have a space in the name.
    $a += @('+runworld', ('"' + $World + '"'))

    Write-Host ("  launching $renderer ...") -ForegroundColor Cyan
    # THE GAME IS DPI-AWARE, WHATEVER WINDOWS DECIDED TONIGHT. At 05:10 on 9 September
    # every capture came back 1.5x its size: the game's window was being scaled by
    # the 150% display, so a 2560x1384 render filled a 3840x2076 window. The layer
    # in the registry says HIGHDPIAWARE and was being ignored; this environment
    # variable is the same layer, applied to this launch only.
    $env:__COMPAT_LAYER = 'HIGHDPIAWARE'
    # ...and the Modernizer's SDL2 makes its own declaration; this is its hint.
    $env:SDL_WINDOWS_DPI_AWARENESS = 'system'
    $p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $a `
                       -WorkingDirectory $Game -PassThru
    for ($s = 0; $s -lt $Wait; $s++) {
        Start-Sleep -Seconds 1
        if ($p.HasExited) { Write-Host "  it exited after $s s" -ForegroundColor Red; return $false }
    }
    # The capture has to happen while the game is up, and window-shot moves and
    # raises the window itself.
    & (Join-Path $PSScriptRoot 'window-shot.ps1') -Out $png -Width $ResW -Height $ResH | ForEach-Object {
        Write-Host ("    " + $_) -ForegroundColor DarkGray }
    try { $p.CloseMainWindow() | Out-Null } catch {}
    Start-Sleep -Seconds 2
    Get-Process lithtech -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
    Start-Sleep -Milliseconds 600
    return (Test-Path $png)
}

Write-Host ""
Write-Host "  $World" -ForegroundColor Yellow

# VRStereo=0 on BOTH arms. The stereo split is done by the CLIENT, not the
# renderer, so a retail-renderer run still comes out as a side-by-side pair and
# is not comparable with a mono one. The first capture taken with this script
# had exactly that fault.
$okR = Run-One 'd3d.ren' 'RETAIL (d3d.ren)' (Join-Path $Out 'retail.png') @('VRStereo=0')
$okO = Run-One 'd3dstub.ren' 'OURS (d3dstub)' (Join-Path $Out 'ours.png') `
        @('StubNativeFrustum=1','VRAsymFrustum=0','VRCrosshair=0','VRStereo=0')

# The renderer's own log for the OURS arm, kept beside its picture.
$live = Join-Path $Game 'logs\renstub.log'
if (Test-Path $live) { Copy-Item $live (Join-Path $Out 'ours-renstub.log') -Force -ErrorAction SilentlyContinue }

# Mean colour of each, which is the cheap tell for "this one drew nothing".
Add-Type -AssemblyName System.Drawing
function Mean-Of([string]$png) {
    if (-not (Test-Path $png)) { return 'missing' }
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    try {
        [double]$r=0; [double]$g=0; [double]$b=0; $n=0
        for ($y = 0; $y -lt $bmp.Height; $y += 8) {
            for ($x = 0; $x -lt $bmp.Width; $x += 8) {
                $c = $bmp.GetPixel($x,$y); $r += $c.R; $g += $c.G; $b += $c.B; $n++
            }
        }
        if (-not $n) { return 'empty' }
        return ("{0,3:N0} {1,3:N0} {2,3:N0}  ({3}x{4})" -f ($r/$n), ($g/$n), ($b/$n), $bmp.Width, $bmp.Height)
    } finally { $bmp.Dispose() }
}

Write-Host ""
Write-Host ("  retail  mean rgb  " + (Mean-Of (Join-Path $Out 'retail.png'))) -ForegroundColor Green
Write-Host ("  ours    mean rgb  " + (Mean-Of (Join-Path $Out 'ours.png'))) -ForegroundColor Green
Write-Host ("  -> $Out") -ForegroundColor Cyan
# Put the player's settings back (see the note at $cfgSnap).
if (Test-Path $cfgSnap) { Copy-Item $cfgSnap $cfgLive -Force; Remove-Item $cfgSnap -Force }
