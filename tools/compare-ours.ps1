# OUR renderer, flat on the monitor. No headset, no host, no stereo.
#
# Run this and tools\compare-retail.ps1 one after the other, walk to the same
# spot in each, and screenshot the same thing. The window title says which build
# you are looking at, so the screenshots label themselves.
#
# Flat and mono on purpose: it takes VR out of the comparison entirely, so a
# difference between the two windows is our RENDERER and nothing else.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

Write-Host ''
Write-Host '  OURS - d3dstub.ren, the renderer we wrote' -ForegroundColor Cyan
Write-Host '  The window is titled "OURS (d3dstub)".' -ForegroundColor Cyan
Write-Host '  It quick-loads straight into the alley. Play normally.' -ForegroundColor Cyan
Write-Host ''

& powershell -File (Join-Path $PSScriptRoot 'run.ps1') `
    -RenderDll 'd3dstub.ren' `
    -Title 'OURS (d3dstub)' `
    -Set 'VRAutoQuickLoad=1,VRStereo=0,StubNativeFrustum=0,VRAsymFrustum=0,VRCrosshair=0'

Write-Host ''
Write-Host "  renderer log: $root\game\logs\renstub.log" -ForegroundColor DarkGray
