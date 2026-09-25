# Runs the game, loads the quick save, and says in one line whether the world
# ever appeared. Built for bisecting the renderer stub against the real d3d.ren.
#
# The verdict is a measurement, not an impression: a working load reaches
# GS_LOADINGLEVEL for ONE frame and then renders the world thousands of times.
# A broken one sits in GS_LOADINGLEVEL for millions of empty frames and renders
# it zero times. Those two are not close together, so there is nothing to judge.
#
#   .\tools\loadtest.ps1 -Renderer d3d.ren
#   .\tools\loadtest.ps1 -Renderer d3dstub.ren -Extra @('StubDelegateLo=1')
#
# Always names the renderer explicitly, because the engine writes RenderDll back
# to autoexec.cfg on exit and a run that does not name one silently inherits
# whatever the last run left. That trap already produced one wrong conclusion
# here; see docs/RENDERSCENE-FIRST-LOOK.md.

param(
    [string]$Renderer = 'd3dstub.ren',
    [string[]]$Extra = @(),
    [int]$Seconds = 25
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent

$set = @("RenderDll=$Renderer", 'ScreenWidth=800', 'ScreenHeight=600',
         'VRAutoQuickLoad=1') + $Extra

& (Join-Path $PSScriptRoot 'probe-run.ps1') -Seconds $Seconds -FakeHost -Set $set | Out-Null

$dir = Get-Content (Join-Path $env:TEMP 'nolfvr-lastrun.txt')
$log = Join-Path $dir 'client.log'
if (-not (Test-Path $log)) { "NO CLIENT LOG - the run did not start"; exit 1 }

$loading = (Select-String -Path $log -Pattern 'GS_LOADINGLEVEL' -SimpleMatch).Count
$world   = (Select-String -Path $log -Pattern 'wr=2' -SimpleMatch).Count

$verdict = if ($world -gt 100) { 'LOADED' } elseif ($world -gt 0) { 'PARTIAL' } else { 'STALLED' }
"{0,-8}  renderer={1,-14} extra={2,-28} world={3,-6} loadingFrames={4}" -f `
    $verdict, $Renderer, ($Extra -join ','), $world, $loading
