# ---------------------------------------------------------------------------
# canopy-retail.ps1 - THE REFERENCE. Unmodified NOLF, nothing of ours in it.
#
#   powershell -ExecutionPolicy Bypass -File tools\canopy-retail.ps1
#
# This answers ONE question: what is the canopy SUPPOSED to look like? Not
# whether it flickers - whether it is there at all, what shape, what colour,
# what it is made of. Our renderer currently keeps BOTH coincident canopy
# brushes out of the draw, so there is a real chance the flashing has been
# replaced by nothing, and a missing canopy passes every flicker test ever
# written. Only the real game can settle that.
#
# IT NEVER TOUCHES YOUR RETAIL INSTALL.
#
# The game writes to its own folder - display settings, config, saves - so it
# is copied to a staging folder first and run from there. Your install at
# <your NOLF install> is opened for READING once, to make that copy, and never again.
# The copy is kept, so the second run starts immediately.
#
# WHAT TO DO
#
#   1. Run it. The game opens windowed, straight into Morocco.
#   2. Walk to the canopy - the one on the sniper roof that flashes.
#   3. STAND AND LOOK AT IT, then keep the view moving a little. The bug only
#      exists between frames; a frozen camera hides it in both builds.
#   4. Press Enter in THIS window. It grabs a burst of frames.
#   5. Do it again from another angle if you like - it re-arms.
#   6. Close the game, or press Q here.
#
# Then run tools\canopy-ours.ps1 and do exactly the same thing in the same
# place. The two folders are what I need.
# ---------------------------------------------------------------------------
param(
    # Your untouched install. Read from, never written to.
    [string]$GamePath = '<your NOLF install>',
    # Which Morocco scene. NOBODY HAS ESTABLISHED WHICH ONE THE SNIPER ROOF IS
    # IN, which is why this is a parameter and not a constant. M01S01 holds
    # Canopy_a/Canopy_b (26 of 26 vertices identical, opposite normals - a
    # two-sided awning built as two brushes). M01S03 holds six more named
    # CanopyTWM*, and TWM means TranslucentWorldModel, which this port draws
    # OPAQUE - a different bug with the same symptom. If the roof is not in the
    # scene you land in, try -Scene M01S03.
    [string]$Scene   = 'M01S01',
    # Or start at the main menu and load your own save instead.
    [switch]$Menu,
    [int]   $Frames  = 24,
    [int]   $ResW    = 1280,
    [int]   $ResH    = 692,
    [string]$Out     = 'logs\canopy\retail',
    # Re-copy the staging folder even if it is already there.
    [switch]$Restage
)

$ErrorActionPreference = 'Stop'
$Root  = Split-Path $PSScriptRoot -Parent
$Stage = Join-Path $Root 'retail-ref'
$OutAbs = Join-Path $Root $Out

Write-Host ''
Write-Host '  RETAIL NOLF - the reference picture' -ForegroundColor Green
Write-Host ''

# ---- the install, read-only --------------------------------------------
if (-not (Test-Path -LiteralPath ([System.IO.Path]::Combine($GamePath, 'lithtech.exe')))) {
    throw ("no lithtech.exe under '{0}' - pass -GamePath with the folder that has it." -f $GamePath)
}

# REFUSE TO RUN THE GAME IN PLACE. The whole point of the staging copy is that
# the original cannot be written to, and a bug here would quietly undo that.
$stageFull = [System.IO.Path]::GetFullPath($Stage)
$gameFull  = [System.IO.Path]::GetFullPath($GamePath)
if ($stageFull.TrimEnd('\') -ieq $gameFull.TrimEnd('\')) {
    throw 'REFUSING: the staging folder and your install are the same folder.'
}

$needStage = $Restage -or -not (Test-Path -LiteralPath ([System.IO.Path]::Combine($Stage, 'lithtech.exe')))
if ($needStage) {
    Write-Host ('  copying your install to {0} (about 1.1 GB, once)...' -f $Stage) -ForegroundColor Cyan
    Write-Host '  Your install is only READ. Nothing is written to it.' -ForegroundColor DarkGray
    New-Item -ItemType Directory -Force $Stage | Out-Null
    $null = & robocopy $GamePath $Stage /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE) - nothing was launched." }
    Write-Host '  copied.' -ForegroundColor Green
} else {
    Write-Host ('  using the staging copy already at {0}' -f $Stage) -ForegroundColor DarkGray
    Write-Host '  (-Restage re-copies it)' -ForegroundColor DarkGray
}

# ---- the launch ---------------------------------------------------------
# The rez list retail itself uses. No Modernizer.rez: this is stock.
$gameArgs = @('-windowtitle', 'RETAIL (stock NOLF)')
foreach ($r in @('NOLF.rez','NOLF2.rez','NOLFdll.rez','NOLFl.rez','custom',
                 'Nolfu003.rez','Nolfcres003.rez','NolfGoty.rez')) {
    if (Test-Path -LiteralPath ([System.IO.Path]::Combine($Stage, $r))) {
        $gameArgs += @('-rez', $r)
    }
}
# +Windowed 1 IS NOT OPTIONAL, and it cost a probe to find out.
#
# Neither the retail config nor ours carries a "Windowed" entry, so it defaults
# to 0 and stock NOLF takes the display EXCLUSIVELY. A GDI capture of a
# fullscreen-exclusive D3D7 surface returns a solid black frame - not an error,
# not a failure, just black - so the first staged launch looked like a renderer
# that would not draw. It was drawing perfectly onto a surface nothing could
# read. Our own build never showed this because d3dstub.ren makes its own
# ordinary window.
$gameArgs += @('+multiplayer', '0', '+Windowed', '1',
               "+ScreenWidth", "$ResW", "+ScreenHeight", "$ResH")

# The same CardDesc fix run.ps1 makes: the engine writes whichever display it
# last used into its config, and after a headset session that names a monitor
# the desktop is no longer on - so the game renders perfectly onto a screen
# nobody is looking at. Forced on the command line, which wins over the file.
Add-Type -AssemblyName System.Windows.Forms
$primary = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Primary } | Select-Object -First 1
if ($primary) {
    $gameArgs += @('+CardDesc', $primary.DeviceName)
    Write-Host ('  display: {0}' -f $primary.DeviceName) -ForegroundColor DarkGray
}

if (-not $Menu) {
    $gameArgs += @('+runworld', ('Worlds\' + $Scene))
    Write-Host ('  loading straight into {0}' -f $Scene) -ForegroundColor Cyan
    Write-Host '  If this is not the scene with the sniper roof, close it and' -ForegroundColor Yellow
    Write-Host '  run again with  -Scene M01S03  (or -Menu to load your own save).' -ForegroundColor Yellow
} else {
    Write-Host '  starting at the main menu - load your own save.' -ForegroundColor Cyan
}

Write-Host ''
$p = Start-Process -FilePath ([System.IO.Path]::Combine($Stage, 'lithtech.exe')) `
                   -ArgumentList $gameArgs -WorkingDirectory $Stage -PassThru
Start-Sleep -Seconds 5
if ($p.HasExited) { throw ("the game exited immediately (code {0})." -f $p.ExitCode) }

# ---- the bursts ---------------------------------------------------------
Write-Host ''
Write-Host '  ============================================================' -ForegroundColor Green
Write-Host '   Walk to the canopy. Look at it. Keep the view moving a bit.' -ForegroundColor Green
Write-Host '   Then come back here and press ENTER to grab a burst.' -ForegroundColor Green
Write-Host '   Q then ENTER when you are done.' -ForegroundColor Green
Write-Host '  ============================================================' -ForegroundColor Green
Write-Host ''

New-Item -ItemType Directory -Force $OutAbs | Out-Null
$burst = 0
while ($true) {
    if ($p.HasExited) { Write-Host '  the game closed.' -ForegroundColor Yellow; break }
    $k = Read-Host 'ENTER to capture, Q to finish'
    if ($k -match '^\s*[qQ]') { break }
    if ($p.HasExited) { Write-Host '  the game closed.' -ForegroundColor Yellow; break }
    $burst++
    Write-Host ("  burst {0}..." -f $burst) -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'canopy-burst.ps1') `
        -Out (Join-Path $OutAbs ("burst{0}" -f $burst)) -Label 'retail' `
        -Count $Frames -Width $ResW -Height $ResH | Out-Host
}

if (-not $p.HasExited) { try { $p.CloseMainWindow() | Out-Null } catch {} }
Start-Sleep -Seconds 2
if (-not $p.HasExited) { try { $p.Kill() } catch {} }

Write-Host ''
Write-Host ('  {0} burst(s) saved under {1}' -f $burst, $OutAbs) -ForegroundColor Green
Write-Host '  Now run tools\canopy-ours.ps1 and do the same thing in the same place.' -ForegroundColor Cyan
Write-Host ''
