# Photographs the game window at intervals WHILE SOMEONE PLAYS, from outside
# the process.
#
# The renderer's own +StubFrameDumpAt reads the back buffer back on the RENDER
# THREAD - a 14 MB GPU readback plus a 10 MB file write - which stalls the
# renderer for a good fraction of a second. LithTech takes one simulation step
# per rendered frame, so each stall hands the engine a huge timestep and every
# enemy in the level moves, aims and fires for all of it at once. Two driving
# sessions were lost to that: a simple handgun sounded like a machine gun and
# killed the player instantly on spawn. The same run with no dumps armed played
# perfectly at 88 FPS.
#
# So: capture from OUTSIDE. window-shot.ps1 -NoMove copies the window's client
# area off the screen and costs the game nothing.
#
# Run this as a BACKGROUND job and the game in the FOREGROUND, never the other
# way round - the fake host silently fails to publish when probe-run.ps1 is
# started from a background job.
#
#   Start-Job { & '<repo>\tools\drive-capture.ps1' -Seconds 240 }
#   .\tools\probe-run.ps1 -Seconds 240 -FakeHost ...

param(
    [int]$Seconds = 240,        # how long to keep capturing
    [double]$Every = 3.0,       # seconds between shots
    [int]$WaitFor = 60,         # how long to wait for the game window
    [string]$Dir = ''
)

$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Dir) {
    $Dir = Join-Path $root ("logs\drive-{0:yyyyMMdd-HHmmss}" -f (Get-Date))
}
New-Item -ItemType Directory -Force $Dir | Out-Null
Write-Host "drive-capture: writing to $Dir"

# Wait for the game, so this can be started before it.
$deadline = (Get-Date).AddSeconds($WaitFor)
while ((Get-Date) -lt $deadline) {
    $g = Get-Process lithtech -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($g -and $g.MainWindowHandle -ne 0) { break }
    Start-Sleep -Milliseconds 500
}
if (-not $g) { Write-Host "drive-capture: no game window appeared"; exit 1 }
Write-Host "drive-capture: game window found, capturing every $Every s"

$shot = Join-Path $PSScriptRoot 'window-shot.ps1'
$n = 0
$stop = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $stop) {
    $g = Get-Process lithtech -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $g) { Write-Host "drive-capture: game exited"; break }
    $n++
    $out = Join-Path $Dir ("drive-{0:d3}.png" -f $n)
    try { & $shot -NoMove -Out $out | Out-Null } catch { }
    Start-Sleep -Milliseconds ([int]($Every * 1000))
}
Write-Host "drive-capture: $n shots in $Dir"
