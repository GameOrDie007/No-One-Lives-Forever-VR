# Starts the x64 capture host, then the game.
#
# Host first: it finishes its graphics setup and then waits for the game's
# window, so there is no race and no need to time the launch by hand.

param([int]$Frames = 600)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent

if (-not (Test-Path (Join-Path $Root 'host\vrhost.exe'))) {
    throw "host\vrhost.exe not found - run tools\build-host.ps1 first."
}

Push-Location $Root
try {
    Start-Process -FilePath (Join-Path $Root 'host\vrhost.exe') `
                  -ArgumentList $Frames -WorkingDirectory $Root
    Start-Sleep -Seconds 2
    & (Join-Path $PSScriptRoot 'run.ps1')
}
finally { Pop-Location }

# The host runs in its own console which closes when it finishes, taking its
# output with it. Everything is on disk, so replay it here.
$run = Get-ChildItem (Join-Path $Root 'logs') -Directory -Filter 'host-*' -EA SilentlyContinue |
       Sort-Object Name | Select-Object -Last 1
if ($run) {
    Write-Host "`n=== host log: $($run.Name) ===" -ForegroundColor Cyan
    Get-Content (Join-Path $run.FullName 'host.log') | ForEach-Object { Write-Host $_ }
}
