# The M5 stability run: 15 minutes continuous, then read the log for what a
# screenshot cannot show.
#
# the project rules has carried "a formal 15-minute unattended stability run has NOT
# been done" since M5 passed in September. Every other check this project makes
# is 24 seconds long, and 24 seconds cannot see a leak, a slow stall, or a
# stereo path that gives up after ten minutes.
#
# THE ONE FAILURE A PICTURE CANNOT CATCH is the mono fallback: if the world
# stops rendering twice per frame the capture at the end looks perfectly
# normal, because a mono frame is a correct picture of one eye. Only the log
# says so, which is why this pairs the run with soak-check.py rather than with
# the usual look at the PNG.
#
# The hands sweep throughout so the VR pose and weapon-publish paths run every
# frame rather than idling - a soak that stands still tests less than a person.
#
#   .\tools\soak.ps1                 15 minutes
#   .\tools\soak.ps1 -Minutes 30
param(
    [int]    $Minutes = 15,
    [string] $Out     = 'logs\soak.png'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$sec  = $Minutes * 60

Write-Host "soak: $Minutes minutes, hands sweeping" -ForegroundColor Cyan
$before = Get-ChildItem (Join-Path $root 'game\logs') -Directory |
          Sort-Object LastWriteTime -Descending | Select-Object -First 1

& (Join-Path $PSScriptRoot 'look-shot.ps1') `
    -Out $Out -Wait $sec -ResW 1280 -ResH 692 `
    -RHandPos "-0.06,1.40,-0.50" -RHandSweep "45,0.5" `
    -Set @('VRCheats=1') | Out-Null

$run = Get-ChildItem (Join-Path $root 'game\logs') -Directory |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($before -and $run.FullName -eq $before.FullName) {
    Write-Host "no new run directory - the client may not have started" -ForegroundColor Red
    exit 1
}
Write-Host ""
& python (Join-Path $PSScriptRoot 'soak-check.py') $run.FullName
$code = $LASTEXITCODE

# The capture is still worth a structure test: a run that stays up for fifteen
# minutes and draws nothing has not passed anything.
Write-Host ""
& python (Join-Path $PSScriptRoot 'check-shots.py') (Split-Path (Join-Path $root $Out) -Parent) 2>&1 |
    Select-Object -First 3
exit $code
