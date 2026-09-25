# Runs the 13 August 2026 CLIENT against TODAY's host.
#
# Two attempts to run the August host failed silently in different ways: the
# first ran today's client because a file swap did not take, and the second
# left the August host waiting for a window it never matched while the client
# never opened the shared block. Neither told us anything about August, and
# both cost a headset session.
#
# So the August host is out of the picture. Today's host has worked all day and
# is the one variable worth holding still. It is told to advertise wire version
# 9 (argv[8]), which is the only thing that stopped the August client attaching
# to it - the contract only ever appends, so every field that client knows
# about sits at the same offset in today's block and reads correctly.
#
# What this tests: the August CLIENT's VR maths - camera composition, per-eye
# frustum centres, the FOV it asks for and publishes. That is where almost all
# of the geometry lives.
#
# What it does NOT test: host-side changes since 13 August, chiefly the
# Catmull-Rom enlargement. If the client turns out to be innocent, that is the
# next thing to hold still.

param([int]$Minutes = 5)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'
$Exe  = Join-Path $Root 'host\nolfvr.exe'
$Rez  = Join-Path $Game 'ModernizerAug13.rez'

foreach ($p in @($Exe, $Rez)) {
    if (-not (Test-Path $p)) { throw "missing $p - run tools\build-aug13.ps1 first." }
}

Get-Process lithtech, nolfvr, 'nolfvr-aug13' -ErrorAction SilentlyContinue |
    ForEach-Object { try { $_.Kill() } catch {} }
Start-Sleep -Seconds 2

# The engine unpacks CShell.dll to %TEMP% and loads it from there, so a stale
# copy left by a crashed run would silently be the wrong client while every
# other line looked right. The August build DOES crash on exit - that fix is
# from 27 August - so this is not hypothetical.
$tmp = Join-Path $env:TEMP 'cshell.dll'
if (Test-Path $tmp) {
    try { Remove-Item $tmp -Force }
    catch { Write-Host "  WARNING: could not remove $tmp - close any running game first" -ForegroundColor Red }
}

Write-Host 'AUGUST CLIENT + today''s host, wire version forced to 9.' -ForegroundColor Yellow
Write-Host '  No auto-quickload: load your quick save from the menu.' -ForegroundColor Yellow
Write-Host '  It will probably crash on exit. Harmless.' -ForegroundColor Yellow

$h = $null
try {
    #                       min   lag  fovY proj same mir  aa  WIRE
    $h = Start-Process -FilePath $Exe `
         -ArgumentList $Minutes, 0, 1.0, 0, 0, 0, 1, 9 `
         -WorkingDirectory $Root -PassThru
    Start-Sleep -Seconds 4

    Start-Job -ScriptBlock {
        $sh = New-Object -ComObject WScript.Shell
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Milliseconds 500
            $g = Get-Process lithtech -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($g -and $g.MainWindowHandle -ne 0) { try { $sh.AppActivate($g.Id) | Out-Null } catch {} }
        }
    } | Out-Null

    & (Join-Path $PSScriptRoot 'run.ps1') -RezName 'ModernizerAug13.rez'
}
finally {
    if ($h -and -not $h.HasExited) { try { $h.Kill() } catch {} }
}

# Prove which client ran and that the two ends actually connected. The absence
# of this check is why two runs today looked like results and were not.
$d = Get-ChildItem (Join-Path $Game 'logs') -Directory | Sort-Object Name | Select-Object -Last 1
Write-Host ''
if ($d) {
    $b = (Select-String -Path (Join-Path $d.FullName 'client.log') -Pattern '^\[.*\] build ' | Select-Object -First 1).Line
    $v = (Select-String -Path (Join-Path $d.FullName 'client.log') -Pattern 'attached to host|out of step' | Select-Object -First 1).Line
    $t = (Select-String -Path (Join-Path $d.FullName 'client.log') -Pattern 'head tracking LIVE' | Select-Object -First 1)

    $okBuild = $b -match '15:32:07'
    Write-Host "client   : $b" -ForegroundColor $(if ($okBuild) { 'Cyan' } else { 'Red' })
    if (-not $okBuild) { Write-Host '           ^ NOT the August client - this run is not the test' -ForegroundColor Red }

    if ($v) { Write-Host "wire     : $v" -ForegroundColor $(if ($v -match 'attached') { 'Cyan' } else { 'Red' }) }
    else    { Write-Host 'wire     : NEVER ATTACHED - this run is not the test' -ForegroundColor Red }

    if ($t) { Write-Host 'tracking : head tracking LIVE' -ForegroundColor Cyan }
    else    { Write-Host 'tracking : NO HEAD TRACKING - this run is not the test' -ForegroundColor Red }
}
