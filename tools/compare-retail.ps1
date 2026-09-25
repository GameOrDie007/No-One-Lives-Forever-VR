# RETAIL renderer, flat on the monitor. This is the reference - what NOLF is
# SUPPOSED to look like. Monolith's own d3d.ren, nothing of ours in the picture.
#
# Same game, same rez files, same quick save, same resolution as
# tools\compare-ours.ps1. Only the renderer differs, which is the whole point.

$ErrorActionPreference = 'Stop'

Write-Host ''
Write-Host '  RETAIL - d3d.ren, the original renderer. This is the target.' -ForegroundColor Green
Write-Host '  The window is titled "RETAIL (d3d)".' -ForegroundColor Green
Write-Host '  It quick-loads straight into the alley. Play normally.' -ForegroundColor Green
Write-Host ''

& powershell -File (Join-Path $PSScriptRoot 'run.ps1') `
    -RenderDll 'd3d.ren' `
    -Title 'RETAIL (d3d)' `
    -Set 'VRAutoQuickLoad=1,VRStereo=0'
