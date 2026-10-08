# The body-quality pass for the mirror: the same mirror, several controller
# poses, one launch each, so the IK arms can be judged pose by pose.
#
#   .\tools\ik-poses.ps1                 # shots\ik\<pose>\last.png + crop.png
#
# Waits for a quiet moment before each launch (another harness may be using
# the machine) and never touches a game it did not start - mirror-tour and
# look-shot both refuse to run over one.

param(
    [string]$World = 'Worlds\M04S01',
    [string]$Stop  = '-3444 208 -40 180',
    [string]$OutRoot = 'shots\ik',
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
Set-Location $Root

$poses = [ordered]@{
    'front'      = @{ R = '';                 L = '';                  Set = @() }
    'sides'      = @{ R = '0.25,0.85,0.0';    L = '-0.25,0.85,0.0';    Set = @() }
    'wide'       = @{ R = '0.6,1.35,-0.1';    L = '-0.6,1.35,-0.1';    Set = @() }
    'reach'      = @{ R = '0.15,1.5,-0.55';   L = '-0.15,1.5,-0.55';   Set = @() }
    'leftorium'  = @{ R = '';                 L = '';                  Set = @('VRLeftorium=1') }
}

function Wait-Quiet {
    $deadline = (Get-Date).AddMinutes(15); $quiet = 0
    while ((Get-Date) -lt $deadline -and $quiet -lt 8) {
        if (Get-Process lithtech -ErrorAction SilentlyContinue) { $quiet = 0 } else { $quiet++ }
        Start-Sleep -Seconds 1
    }
    return ($quiet -ge 8)
}

Add-Type -AssemblyName System.Drawing
foreach ($name in $poses.Keys) {
    if ($Only.Count -and $Only -notcontains $name) { continue }
    $p = $poses[$name]
    if (-not (Wait-Quiet)) { Write-Host "  $name : no quiet gap in 15 minutes - skipped" -ForegroundColor Red; continue }
    $args = @{ World = $World; Stops = @($Stop); Out = (Join-Path $OutRoot $name); ResW = 3840; ResH = 2076
               Set = (@('StubMirrorBody=1','VRShowBody=1','StubBody=1') + $p.Set) }
    if ($p.R) { $args['RHandPos'] = $p.R }
    if ($p.L) { $args['LHandPos'] = $p.L }
    & (Join-Path $PSScriptRoot 'mirror-tour.ps1') @args *>&1 | Out-Null
    $png = Join-Path $Root (Join-Path $OutRoot "$name\last.png")
    if (Test-Path $png) { Write-Host "  $name : $png" } else { Write-Host "  $name : NO PICTURE" -ForegroundColor Red }
}
