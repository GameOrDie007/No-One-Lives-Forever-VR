# ---------------------------------------------------------------------------
# night-battery.ps1 - the five questions left open on the night of 11 September,
# each as one run, each printing the line that answers it.
#
# Every one of these enters the world through the QUICK SAVE rather than
# +runworld. That is not a detail: a level started with +runworld keeps its
# opening camera live for the whole level, which puts the player camera in
# CHASE and leaves the view weapon hidden and DISABLED - a state in which there
# is no muzzle flash, no weapon animation and no first-person effects at all.
# Three runs were spent measuring that before it was understood.
#
#   .\tools\night-battery.ps1              all five
#   .\tools\night-battery.ps1 -Only flash
# ---------------------------------------------------------------------------
param(
    [ValidateSet('all','build','flash','lights','eyes','saveload','sound')]
    [string]$Only = 'all'
)

$ErrorActionPreference = 'Continue'
$Root = Split-Path $PSScriptRoot -Parent
function Head($s) { Write-Host ''; Write-Host ("=== " + $s + " ===") -ForegroundColor Magenta }
function LastLog { (Get-ChildItem (Join-Path $Root 'game\logs') -Directory |
                    Sort-Object Name | Select-Object -Last 1).FullName }
function Show($path, $pattern, $n) {
    $f = Join-Path $path 'client.log'
    if (-not (Test-Path $f)) { Write-Host '   no client log' -ForegroundColor Red; return }
    $hits = Get-Content $f | Select-String $pattern | Select-Object -Last $n
    if ($hits) { $hits | ForEach-Object { '   ' + $_.Line.Trim() } }
    else { Write-Host ("   nothing matched /" + $pattern + "/") -ForegroundColor Yellow }
}

if ($Only -in 'all','build') {
    Head 'build'
    & (Join-Path $PSScriptRoot 'build.ps1') 2>&1 |
        Select-String -Pattern ': error|Build complete' | ForEach-Object { '   ' + $_.Line }
}

if ($Only -in 'all','flash') {
    Head 'the muzzle flash - is the object that is switched on the one we publish?'
    & (Join-Path $PSScriptRoot 'look-shot.ps1') -Wait 18 -KeyDelay 14 -Keys '2' `
        -Set 'VRCheats=7;VRDebugWeapon=1' -Buttons 1 -ButtonsHand right -ButtonsAt 20 `
        -RHand '25,0,0' -Out 'logs\battery-flash.png' | Out-Null
    $d = LastLog
    Show $d 'VRDebugWeapon [0-9]|has a silencer fitted' 3
    Show $d 'VRFlash:' 1
    Show $d 'VRExtra:' 2
    Show $d 'handed over:' 1
}

if ($Only -in 'all','lights') {
    Head 'dynamic lights - the flashlight makes one, does the renderer get it?'
    # The LEFT trigger is bound to the flashlight and the client edge-triggers,
    # so a held button fires its one edge and the light stays on.
    & (Join-Path $PSScriptRoot 'look-shot.ps1') -Wait 22 `
        -Buttons 1 -ButtonsHand left -ButtonsAt 8 -Out 'logs\battery-light.png' | Out-Null
    $d = LastLog
    Show $d 'VRControls: first fire of command 63' 1
    Show $d 'dynamic lights now' 2
}

if ($Only -in 'all','eyes') {
    Head 'do the two eyes agree when they are at the same point?'
    & (Join-Path $PSScriptRoot 'eye-agree.ps1') -Wait 18 | Out-Host
}

if ($Only -in 'all','saveload') {
    Head 'a quick load, after another level - the third way into a world'
    & (Join-Path $PSScriptRoot 'saveload-test.ps1') | Out-Host
}

if ($Only -in 'all','sound') {
    Head 'a 3D sound provider - does the game start and survive a transition?'
    & (Join-Path $PSScriptRoot 'chain-levels.ps1') -Missions 'M01' -OutDir 'logs\sound3d' `
        -Set 'VRSoundProvider=DirectSound3D 7+ Software - Full HRTF' | Out-Host
    $d = LastLog
    Show $d 'VRSound:' 4
}

Write-Host ''
Write-Host 'battery done.' -ForegroundColor Green
