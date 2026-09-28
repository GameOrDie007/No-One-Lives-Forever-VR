# Photograph a level's mirrors at the desk: stand in front of each one in turn.
#
#   .\tools\mirror-tour.ps1 -World Worlds\M04S01 -Stops '-3444 208 37 180','-3444 208 -287 0' -Out shots\mirrors\M04S01
#
# ONE stop, "x y z yaw" (vrtour.txt; yaw 0 looks down +z, 90 down +x): the
# engine teleport can only be given once per run, so one viewpoint per launch.
# tools\find-mirrors.py prints stops for the mirrors on world models; the
# renderer's log ("R3D MIRROR: face ...") gives the rest. Needs the DEBUG client
# (VRTour is a debug tool). The frames land in -Out with the renderer and
# client logs; game\vrtour.txt is removed afterwards whatever happens.
#
# With no -Stops it just loads the level and dumps frames: that run's
# renderer log lists every mirror face, which is where the stops come from.

param(
    [Parameter(Mandatory)][string]$World,
    [string[]]$Stops = @(),
    [Parameter(Mandatory)][string]$Out,
    [int]$StopSeconds = 10,
    [int]$DumpEvery = 300,
    [string]$Look = '0,0,0',
    [string[]]$Set = @(),
    [int]$ResW = 2560,
    [int]$ResH = 1384,
    [string]$RHandPos = '',
    [string]$LHandPos = '',
    [string[]]$Keys = @(),
    [switch]$Sweep
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'
$tour = Join-Path $Game 'vrtour.txt'
$OutDir = Join-Path $Root $Out
New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $Game\logs -Filter 'frame-*.bmp' -ErrorAction SilentlyContinue | Remove-Item -Force

# One stop list per run; several strings joined by commas from bash arrive as one.
$list = @(); foreach ($s in $Stops) { $list += ($s -split ',') | Where-Object { $_.Trim() } }
if ($list.Count -gt 1) { Write-Host "  one viewpoint per run: using the first stop only" -ForegroundColor Yellow; $list = @($list[0]) }
$wait = 22 + $StopSeconds * [Math]::Max(1, $list.Count)
$sets = @("StubFrameDumpEvery=$DumpEvery") + $Set
try {
    if ($list.Count) {
        Set-Content -Path $tour -Value $list -Encoding ASCII
        $sets += @('VRTour=1', "VRTourEvery=$StopSeconds")
    }
    # THE POSITION COMES FROM THE ENGINE'S TELEPORT (-At). VRTour's own move is
    # client-side: the server keeps the player where he was and the camera
    # follows the server, so a tour-only run photographs the spawn point with
    # the yaw turned (27 Sep, M04S01). One viewpoint per run, then: -At for
    # where, the one-stop tour for which way.
    $at = @{}
    if ($list.Count) {
        $x, $y, $z, $null = ($list[0].Trim() -split '\s+')
        $at = @{ At = "$x,$y,$z"; AtDelay = 6 }
    }
    if ($RHandPos) { $at['RHandPos'] = $RHandPos }
    if ($LHandPos) { $at['LHandPos'] = $LHandPos }
    if ($Keys.Count) { $at['Keys'] = $Keys; $at['KeyDelay'] = 4 }
    if ($Sweep) { $at['Sweep'] = $true }
    & (Join-Path $PSScriptRoot 'look-shot.ps1') -World $World -Wait $wait -Look $Look `
        -Set $sets -Out (Join-Path $Out 'last.png') -ResW $ResW -ResH $ResH @at | Out-Null
}
finally {
    if (Test-Path $tour) { Remove-Item $tour -Force }
}
Get-ChildItem $Game\logs -Filter 'frame-*.bmp' -ErrorAction SilentlyContinue | Move-Item -Destination $OutDir -Force
$run = Get-ChildItem $Game\logs -Directory | Sort-Object Name | Select-Object -Last 1
if ($run -and (Test-Path (Join-Path $run.FullName 'client.log'))) {
    Copy-Item (Join-Path $run.FullName 'client.log') (Join-Path $OutDir 'client.log') -Force
}
$rl = Join-Path $OutDir 'last-renstub.log'
$faces = if (Test-Path $rl) { @(Select-String -Path $rl -Pattern 'R3D MIRROR: (face|\d+ reflecting)' | ForEach-Object { $_.Line.Trim() } | Select-Object -Unique) } else { @() }
$tourLines = if (Test-Path (Join-Path $OutDir 'client.log')) { @(Select-String -Path (Join-Path $OutDir 'client.log') -Pattern 'VRTour:' | ForEach-Object { $_.Line.Trim() }) } else { @() }
"$World - $(@(Get-ChildItem $OutDir -Filter 'frame-*.bmp').Count) frames"
$faces | Select-Object -First 30
$tourLines | Select-Object -First 12
