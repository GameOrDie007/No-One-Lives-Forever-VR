# Measures what d3d.ren actually rasterises on the VERTICAL axis.
#
# The horizontal was settled by rendering the same scene at a series of known
# YAWS across consecutive normal frames and correlating how far the image moved
# (docs/FIELD-MEASURED-PROPERLY.md, tan ratio 0.993). Nothing has ever done the
# same on the vertical - "The correlation yaws; it never pitches."
#
# This is that measurement, with the head doing the pitching instead of the
# camera, so it goes through the exact composition the game uses.
#
# One short run per pitch angle, each loading the SAME quick save, so the scene
# is identical and the only difference between captures is the pitch. Simpler
# and more robust than stepping a pose mid-run and trying to capture in time
# with it - there is no timing to get wrong.
#
# The capture is the LEFT eye only. The right eye is clipped by the desktop and
# is not needed: this measures one axis of one eye.

param(
    [string]$Angles = '0,4,8,12',
    [int]$Settle = 26,
    [int]$EyeW = 1440,
    [int]$EyeH = 1440
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Out  = Join-Path $Root 'logs\vfield'
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null

$list = $Angles -split ','
foreach ($a in $list) {
    Write-Host "=== pitch $a deg ===" -ForegroundColor Cyan

    $job = Start-Process -FilePath 'powershell' -PassThru -WindowStyle Minimized `
        -ArgumentList '-NoProfile', '-Command',
        ("& '$PSScriptRoot\probe-run.ps1' -Seconds $($Settle + 8) -FakeHost " +
         "-FakeHostArgs '--static 0,$a,0 --quiet' -Set @('VRAutoQuickLoad=1')")

    Start-Sleep -Seconds $Settle

    $png = Join-Path $Out ("pitch-{0}.png" -f $a)
    & (Join-Path $PSScriptRoot 'shot.ps1') -W $EyeW -H $EyeH -Scale 1 -Out $png | Out-Null

    # Let the run finish and close the game itself, so the next launch is not
    # fighting a process that still holds the shared block.
    while (-not $job.HasExited) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 1
}

Write-Host "`ncaptures in $Out" -ForegroundColor Green
Get-ChildItem $Out | ForEach-Object { "  $($_.Name)  $($_.Length) bytes" }
