# Launches the staged game, lets it run for a fixed number of seconds, then
# closes it - so a measurement that prints into the client log can be taken with
# nobody watching and no headset connected.
#
# Everything this project needs to check about the camera composition is a pure
# function of two rotations. It needs the ENGINE (so LTRotation's conventions
# are the real ones) but it does not need a headset, a runtime, or a person.
# This is the harness that runs it.
#
#   -Seconds   how long to let the game live (default 35)
#   -Set       console variables, same form as run.ps1: -Set VRAutoQuickLoad=1
#   -FakeHost  publish a synthetic head pose into the shared block, so the VR
#              paths that require a live host actually execute
#
# CALL IT WITH & AND A REAL ARRAY when setting more than one variable:
#
#     & .\tools\probe-run.ps1 -Seconds 60 -FakeHost -Set @('VRAutoQuickLoad=1','VRFovXTest=0.8')
#
# Launching it with `powershell -File ... -Set a=1,b=2` passes the whole thing
# as ONE string, so only the first name is set and the rest end up inside its
# value. It prints what it set, and the printed line is what actually happened -
# a run whose second variable silently did not apply already cost one wrong
# A/B here.
#
# Refuses to start while any lithtech.exe is running (it may be a player's), and
# closes only the process it started, verifying it is gone before returning.

param(
    [int]$Seconds = 35,
    [string[]]$Set = @(),
    [switch]$FakeHost,
    [string]$FakeHostArgs = '',
    [string]$RezName = 'Modernizer.rez'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'

# Nothing else may be holding the game folder or the shared block.
# NEVER KILL A GAME THIS SCRIPT DID NOT START. A running lithtech.exe is most
# likely a player's session; stop and say so instead of closing it.
if (Get-Process lithtech -ErrorAction SilentlyContinue) {
    Write-Host '  STOPPED: the game is already running (a player may be in it). Close it first; nothing was touched.' -ForegroundColor Red
    exit 1
}

$before = Get-ChildItem (Join-Path $Game 'logs') -Directory -ErrorAction SilentlyContinue |
          ForEach-Object { $_.Name }

$fake = $null
if ($FakeHost) {
    $py = Join-Path $PSScriptRoot 'fakehost.py'
    Write-Host "starting the fake host ($FakeHostArgs)" -ForegroundColor Cyan
    $fakeArgs = @('"' + $py + '"')   # quoted: Start-Process quotes nothing (a spaced path split)
    if ($FakeHostArgs) { $fakeArgs += ($FakeHostArgs -split ' ') }
    $fakeLog = Join-Path $Root 'logs\fakehost.log'
    New-Item -ItemType Directory -Force (Split-Path $fakeLog) | Out-Null
    $fake = Start-Process -FilePath 'python' -ArgumentList $fakeArgs -PassThru `
                          -WindowStyle Minimized -RedirectStandardOutput $fakeLog
    Start-Sleep -Seconds 2
    if ($fake.HasExited) { throw "fake host exited immediately (exit $($fake.ExitCode))" }
}

$rez = @(
    'NOLF.rez', 'NOLF2.rez', 'NOLFdll.rez', 'NOLFl.rez', 'custom',
    'Nolfu003.rez', 'Nolfcres003.rez', 'NolfGoty.rez',
    $RezName
)
$gameArgs = @('-windowtitle', 'NOLF VR')
foreach ($r in $rez) { $gameArgs += @('-rez', $r) }
$gameArgs += @('+multiplayer', '0')

# Same CardDesc override as run.ps1, and for the same reason - see the comment
# there. This script does not go through run.ps1, so it needs its own copy;
# without it a desk run and a headset run can be pointed at different displays
# while looking identical on the command line.
Add-Type -AssemblyName System.Windows.Forms
$primary = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Primary } |
           Select-Object -First 1
if ($primary) {
    $gameArgs += @('+CardDesc', $primary.DeviceName)
    Write-Host ("display: {0} ({1}x{2})" -f $primary.DeviceName,
                $primary.Bounds.Width, $primary.Bounds.Height) -ForegroundColor Cyan
}
# ALWAYS name the renderer.
#
# The engine writes its console variables back to autoexec.cfg when it exits,
# so a run that passed +RenderDll d3dstub.ren leaves "RenderDll" "d3dstub.ren"
# in the file - and every later run that does NOT name a renderer silently
# inherits it. That produced a "stock renderer control" here which was actually
# running the stub, and the wrong conclusion it supported ("the level-load
# stall is pre-existing") was written into a doc and committed before the real
# control was run.
#
# So: unless the caller names one, this passes the stock renderer explicitly.
# A run must state what it ran, and the printed line below is that statement.
# Accept a COMMA-SEPARATED -Set as well as an array. The header above has
# warned since the beginning that `-File ... -Set a=1,b=2` binds as ONE string
# and silently sets only the first name; run.ps1 now splits it, and so must
# this, because probe-run has its own -Set loop and the fix to the other one
# looked like it covered both. It did not - verified by running it.
#
# Only split where BOTH sides look like settings, so a value that legitimately
# contains a comma is left alone.
$expanded = @()
foreach ($s in $Set) {
    if ($s -match '^\s*\w+\s*=[^,]*,\s*\w+\s*=') { $expanded += ($s -split ',') }
    else { $expanded += $s }
}
$Set = @($expanded | Where-Object { $_ -and $_.Trim() } | ForEach-Object { $_.Trim() })

if (-not ($Set | Where-Object { $_ -match '^\s*RenderDll\s*=' })) {
    $Set = @('RenderDll=d3d.ren') + $Set
}

foreach ($s in $Set) {
    $name, $value = $s -split '=', 2
    if (-not $value) { throw "-Set expects Name=Value, got '$s'" }
    $gameArgs += @("+$name", $value)
    Write-Host "console: $name = $value" -ForegroundColor Cyan
}

Write-Host "launching for $Seconds s..." -ForegroundColor Cyan
$p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $gameArgs `
                   -WorkingDirectory $Game -PassThru

# The window is left where the engine puts it.
#
# This used to move the game window onto one display with SetWindowPos from a
# background job, on the theory that a window straddling monitors escapes the
# compositor's throttle and free-runs. The throttle is no longer what holds the
# frame rate - the renderer's own cap does, and it is measured holding exactly
# 342 presents per 3.8 s block (90.0/s, dead constant) regardless of where the
# window sits. So the move buys nothing, and it costs: moving a window out from
# under a game that has captured and clipped the cursor desynchronises the clip
# rectangle. Do not reintroduce it. If placement ever matters again, do it
# BEFORE the game captures the mouse, not from a job half a second in.

for ($i = 0; $i -lt $Seconds; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) {
        $code = $p.ExitCode
        Write-Host ("game exited on its own after {0} s, exit code {1} (0x{1:X8})" -f $i, $code) -ForegroundColor Yellow
        break
    }
}

if (-not $p.HasExited) {
    Write-Host 'closing the game...' -ForegroundColor Cyan
    try { $p.CloseMainWindow() | Out-Null } catch {}
    Start-Sleep -Seconds 3
}
# Only the process this script started.
if (-not $p.HasExited) { try { $p.Kill() } catch {}; $p.WaitForExit(5000) | Out-Null }
Start-Sleep -Seconds 1

if ($fake -and -not $fake.HasExited) { try { $fake.Kill() } catch {} }

if (-not $p.HasExited) { Write-Host "WARNING: lithtech.exe (pid $($p.Id)) is still running" -ForegroundColor Red }

$after = Get-ChildItem (Join-Path $Game 'logs') -Directory -ErrorAction SilentlyContinue |
         Where-Object { $before -notcontains $_.Name } | Sort-Object Name
if (-not $after) {
    Write-Host 'NO NEW LOG DIRECTORY - the client shell never initialised.' -ForegroundColor Red
    exit 1
}
$dir = ($after | Select-Object -Last 1).FullName

# Archive the RENDERER log beside the client one.
#
# game/logs/renstub.log is opened with "w" on every launch, so the next
# run destroys it. A run that exited on its own at 11 seconds had its
# renderer log overwritten by the very next run started to compare
# against it, and the only copy of the evidence went with it.
foreach ($extra in @('renstub.log', 'renshim.log')) {
    $src = Join-Path $Game "logs\$extra"
    if (Test-Path $src) { Copy-Item $src (Join-Path $dir $extra) -Force }
}

Write-Host "log: $dir" -ForegroundColor Green
$dir | Out-File -FilePath (Join-Path $env:TEMP 'nolfvr-lastrun.txt') -Encoding utf8
