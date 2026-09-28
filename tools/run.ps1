# Launches the staged NOLF build directly, bypassing NOLF.exe (the retail launcher).
# Going straight to lithtech.exe gives us an explicit, reproducible rez load order.
#
# -Set takes console variables to apply at startup, e.g. -Set VRHeadAsMouse=1.
# The engine creates them before the client shell runs, and VarTrack::Init only
# writes its default when the variable does not already exist - so a value set
# here wins over the compiled-in default and holds for the whole run.
#
# This exists because a mode that can only be reached by pressing a key in the
# headset can silently not be reached at all: a run was judged and reported on
# the assumption F1 had been pressed when the log showed it had not.

# -RezName picks which client rez is loaded last, and therefore which CShell.dll
# wins. It exists so an older build can be run side by side with today's WITHOUT
# copying files over each other: the first attempt at that swapped
# Modernizer.rez in and out around the run, the swap silently did not take, and
# a whole headset session ran a current client against an old host
# - which refused to attach, so there was simply no VR and nothing to see.
#
# Loading by name cannot half-happen. The log's "build :" line names the DLL
# that actually got loaded, and that is the only proof worth having.
param([string[]]$Set = @(), [string]$RezName = 'Modernizer.rez',
      [string]$RenderDll = 'd3d.ren',
      [int]$Width = 3840, [int]$Height = 2076,
      [string]$Title = 'No One Lives Forever VR')

$ErrorActionPreference = 'Stop'

$Game = Join-Path (Split-Path $PSScriptRoot -Parent) 'game'

# Load order matters: later rez files override earlier ones, so Modernizer.rez is last.
# WidescreenGOTY.rez is deliberately absent - it carries a conflicting CShell.dll.
$rez = @(
    'NOLF.rez', 'NOLF2.rez', 'NOLFdll.rez', 'NOLFl.rez', 'custom',
    'Nolfu003.rez', 'Nolfcres003.rez', 'NolfGoty.rez',
    $RezName
)

# THE UPSCALED GUN TEXTURES, LAST, SO THEY WIN. packs\sub-guns.rez is the
# GUNS subset of the ESRGAN pack - 78 files, 285 MB, player-view skins from
# 256x256 to 1024x1024 - and it was the one subset the bisect found clean
# (docs/UPSCALE-PACK.md): CHARS is what exhausts the 32-bit process, and
# this is not CHARS. A FILE outside the game folder mounts fine; it is a
# DIRECTORY outside it that silently mounts nothing. Skipped, with a line,
# when the file is not there, so a checkout without the packs still runs.
$guns = Join-Path (Split-Path $Game -Parent) 'packs\sub-guns.rez'
if (Test-Path $guns) {
    $rez += '..\packs\sub-guns.rez'
    Write-Host 'textures: upscaled gun skins mounted (packs\sub-guns.rez)' -ForegroundColor Cyan
} else {
    Write-Host 'textures: packs\sub-guns.rez not present - retail gun skins' -ForegroundColor DarkGray
}

$gameArgs = @('-windowtitle', $Title)
foreach ($r in $rez) { $gameArgs += @('-rez', $r) }
$gameArgs += @('+multiplayer', '0')

# Force CardDesc to whatever is ACTUALLY the primary display, every launch.
#
# The engine rewrites autoexec.cfg on exit with the display it happened to be
# using, and the display SET changes when the headset connects and disconnects.
# So a VR session ends by writing a CardDesc that names a monitor which is no
# longer where the desktop is - and the next launch renders, perfectly, onto a
# display nobody is looking at. That is docs/BLACK-WINDOW.md, and it has now
# cost this project three separate diagnoses of a "rendering bug".
#
# Passed on the command line rather than written into the config, because the
# engine owns that file and will overwrite any fix on the next exit. This wins
# for the run regardless of what the file says.
Add-Type -AssemblyName System.Windows.Forms
$primary = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Primary } |
           Select-Object -First 1
if ($primary) {
    $gameArgs += @('+CardDesc', $primary.DeviceName)
    Write-Host ("display: {0} ({1}x{2})" -f $primary.DeviceName,
                $primary.Bounds.Width, $primary.Bounds.Height) -ForegroundColor Cyan

    $cfg = Join-Path $Game 'autoexec.cfg'
    if (Test-Path $cfg) {
        $stale = Select-String -Path $cfg -Pattern '^"CardDesc"' | Select-Object -First 1
        if ($stale -and $stale.Line -notlike "*$($primary.DeviceName)*") {
            Write-Host ("  autoexec.cfg still says {0} - overriding for this run" -f
                        $stale.Line.Trim()) -ForegroundColor Yellow
        }
    }
}

# Name the renderer, every launch, whatever the file says.
#
# The engine writes console variables back to autoexec.cfg on exit, so a
# launch that does not name one silently inherits whatever the last run
# used - and the last run is often a desk probe of the stub renderer. A run
# that cannot say what it ran is not a measurement; probe-run.ps1 has done
# this since the day a 'stock renderer control' turned out to be running the
# stub and its conclusion was committed.
if (-not ($Set | Where-Object { $_ -match '^\s*RenderDll\s*=' })) {
    $Set = @("RenderDll=$RenderDll") + $Set
}

# Force the RESOLUTION on the command line too, for the same reason as CardDesc
# above: the engine rewrites autoexec.cfg on exit, and it writes whatever it
# ended up using - including a safe-mode fallback after a bad launch.
#
# That happened. A run with a malformed +RenderDll left the engine in 800x600,
# it saved that, and the next HEADSET session came back "extremely low
# resolution". The config had been 2560x1384 twenty minutes earlier. A setting
# the engine owns and rewrites cannot be left in the file and trusted.
#
# 3840x2076 is 1920x2076 per eye, which is the 0.925 aspect the native per-eye
# frustum wants. tools/set-res.ps1 prints the arithmetic.
#
# RAISED FROM 2560x1384 ON 8 SEPTEMBER. Headset testing reported far too much
# aliasing and a low-res feel. The runtime asks for 3072x3264 per eye and was
# being handed 1280x1384 - a 2.4x upscale, which is most of both complaints.
#
# It is very nearly free, because this game is not fill-rate bound at all.
# Desk-measured, same scene, same build:
#
#   2560x1384   85.0 fps      3840x2076   84.5 fps   <- 2.25x the pixels
#   4608x2492   80.0 fps      6144x3322   58.0 fps
#
# 3840 is the last step that costs nothing measurable. The window is clamped to
# the desktop (client area comes back 3860 wide) but the BACK BUFFER is the
# mode size, so the render target really is 3840x2076 - the depth buffer line
# in the renderer log confirms it independently.
if (-not ($Set | Where-Object { $_ -match '^\s*ScreenWidth\s*=' })) {
    $Set = @("ScreenWidth=$Width", "ScreenHeight=$Height") + $Set
    Write-Host "resolution: ${Width}x${Height} ($($Width / 2)x$Height per eye)" -ForegroundColor Cyan
}

# Accept a COMMA-SEPARATED -Set as well as an array.
#
# `powershell -File ... -Set a=1,b=2` binds the whole thing as ONE string, so
# only the first name was set and every other pair ended up inside its value -
# silently, which already cost one wrong conclusion. And `-Set @('a=1','b=2')`
# does not work under -File at all: @() is not evaluated, the tokens split, and
# the second one binds to whatever positional parameter comes next (it landed on
# -Minutes and threw a type error). Splitting here makes every form work, at the
# one choke point both scripts pass through.
#
# Only split where BOTH sides look like settings, so a value that legitimately
# contains a comma is left alone.
$expanded = @()
foreach ($s in $Set) {
    if ($s -match '^\s*\w+\s*=[^,]*,\s*\w+\s*=') { $expanded += ($s -split ',') }
    else { $expanded += $s }
}
$Set = $expanded | Where-Object { $_ -and $_.Trim() } | ForEach-Object { $_.Trim() }

foreach ($s in $Set) {
    $name, $value = $s -split '=', 2
    if (-not $value) { throw "-Set expects Name=Value, got '$s'" }
    # QUOTE A VALUE WITH SPACES IN IT. -ArgumentList joins an array with
    # spaces and does NOT quote, so `runworld` for a level called
    # "GUNS & HOCKEYPUCKS_DM" arrives as three separate arguments and the
    # engine shuts down on an unknown world. Every sweep script already
    # works around this locally; doing it here fixes it for all of them.
    if ($value -match '\s' -and $value -notmatch '^".*"$') {
        $gameArgs += @("+$name", ('"' + $value + '"'))
    } else {
        $gameArgs += @("+$name", $value)
    }
    Write-Host "console: $name = $value" -ForegroundColor Cyan
}

# Which client log directories exist BEFORE the run, so the one this run
# creates can be identified afterwards.
$before = Get-ChildItem (Join-Path $Game 'logs') -Directory -ErrorAction SilentlyContinue |
          Select-Object -ExpandProperty Name

Push-Location $Game
try {
    & (Join-Path $Game 'lithtech.exe') @gameArgs
} finally {
    Pop-Location
}

# Archive the RENDERER log beside the client one.
#
# game/logs/renstub.log is opened with "w" on every launch, so the next run
# destroys it. probe-run.ps1 has done this for a while; run.ps1 did not - and
# run.ps1 is the path every HEADSET session takes. A play session was described,
# the game relaunched to look into it, and the only copy of that session's
# renderer log went with the relaunch.
$after = Get-ChildItem (Join-Path $Game 'logs') -Directory -ErrorAction SilentlyContinue |
         Where-Object { $before -notcontains $_.Name } | Sort-Object Name
if ($after) {
    $dir = ($after | Select-Object -Last 1).FullName
    foreach ($extra in @('renstub.log', 'renshim.log')) {
        $src = Join-Path $Game "logs\$extra"
        # Copy with SHARED access: a VR quit can leave the game holding the file
        # (see the zombie-process note), and Copy-Item then fails outright.
        if (Test-Path $src) {
            try {
                $fs = [System.IO.File]::Open($src, [System.IO.FileMode]::Open,
                        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                $sr = New-Object System.IO.StreamReader($fs)
                $txt = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
                Set-Content -Path (Join-Path $dir $extra) -Value $txt -Encoding utf8
            } catch {
                Write-Host "  could not archive $extra : $_" -ForegroundColor Yellow
            }
        }
    }
    Write-Host "log: $dir" -ForegroundColor Green
}
else {
    Write-Host 'NO NEW LOG DIRECTORY - the client shell never initialised.' -ForegroundColor Red
}
