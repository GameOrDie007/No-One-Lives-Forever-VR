<#
    No One Lives Forever VR - collect a report. Double-click "Collect report.bat" after a
    session that went wrong and send the zip it puts on your Desktop.

    It holds the logs of the most recent sessions and the game's settings file.
    Nothing else: no saves, no game files.
#>
param([int]$Sessions = 2)

$ErrorActionPreference = "Stop"
$pkg  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$game = [System.IO.Path]::Combine($pkg, "game")

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stage = [System.IO.Path]::Combine($env:TEMP, "nolfvr-report-$stamp")
[System.IO.Directory]::CreateDirectory($stage) | Out-Null

function Take($path, $as) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    $dst = [System.IO.Path]::Combine($stage, $as)
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $dst)) | Out-Null
    if ((Get-Item -LiteralPath $path).PSIsContainer) {
        Copy-Item -LiteralPath $path -Destination $dst -Recurse -Force
    } else {
        Copy-Item -LiteralPath $path -Destination $dst -Force
    }
}

# The host's logs (the headset side) and the game's (the client side).
$hostLogs = [System.IO.Path]::Combine($pkg, "logs")
if (Test-Path -LiteralPath $hostLogs) {
    Get-ChildItem -LiteralPath $hostLogs -Directory -Filter "vrhost-*" | Sort-Object LastWriteTime -Descending |
        Select-Object -First $Sessions | ForEach-Object { Take $_.FullName ("host\" + $_.Name) }
}
$gameLogs = [System.IO.Path]::Combine($game, "logs")
if (Test-Path -LiteralPath $gameLogs) {
    Get-ChildItem -LiteralPath $gameLogs -Directory | Sort-Object LastWriteTime -Descending |
        Select-Object -First $Sessions | ForEach-Object { Take $_.FullName ("game\" + $_.Name) }
    Take ([System.IO.Path]::Combine($gameLogs, "renstub.log")) "game\renstub.log"
}
Take ([System.IO.Path]::Combine($game, "autoexec.cfg")) "game\autoexec.cfg"
Take ([System.IO.Path]::Combine($pkg, "version.txt")) "version.txt"

$desk = [Environment]::GetFolderPath("Desktop")
$zip  = [System.IO.Path]::Combine($desk, "NOLF-VR-report-$stamp.zip")
Compress-Archive -Path ([System.IO.Path]::Combine($stage, "*")) -DestinationPath $zip -CompressionLevel Optimal
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Host ""
Write-Host "Report saved: $zip" -ForegroundColor Green
Write-Host ""
