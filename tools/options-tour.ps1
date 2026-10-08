# Photograph every VR Options page, the Display page and the pause menu, from
# the desk, in one launch.
#
# The debug client's VRDebugOpenVRPage 2 opens each page in turn, six seconds
# apart, and logs "tour page N of M" as it does. This script launches the game
# the way menu-click.ps1 does (the fake host, so the VR pages behave as in a
# headset), waits for each of those lines in the client log, and captures the
# window with window-shot.ps1 a moment later. What it proves: every row of
# every page is there and fits - which is what a headset round should not be
# spent finding out.
#
#   .\tools\options-tour.ps1          # PNGs in logs\options-tour

param(
    [string]  $Out = 'logs\options-tour',
    [int]     $Timeout = 120
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'
# NEVER KILL A GAME THIS SCRIPT DID NOT START.
if (Get-Process lithtech -ErrorAction SilentlyContinue) {
    Write-Host '  STOPPED: the game is already running (a player may be in it). Close it first; nothing was touched.' -ForegroundColor Red
    exit 1
}
$OutDir = Join-Path $Root $Out
New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $OutDir -Filter '*.png' -ErrorAction SilentlyContinue | Remove-Item -Force

Add-Type -AssemblyName System.Windows.Forms

# Only a leftover fake host, never every python on the machine.
$myFake = Join-Path $PSScriptRoot 'fakehost.py'
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($myFake) } |
    ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force } catch {} }
Start-Sleep -Milliseconds 800

$fakeLog = Join-Path $Root 'logs\fakehost-tour.log'
$fake = Start-Process -FilePath 'python' `
        -ArgumentList @(('"' + (Join-Path $PSScriptRoot 'fakehost.py') + '"'), '--static', '0,0,0') `
        -PassThru -WindowStyle Minimized -RedirectStandardOutput $fakeLog
Start-Sleep -Seconds 2
if ($fake.HasExited) { throw "fake host exited (exit $($fake.ExitCode))" }

$primary = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Primary } | Select-Object -First 1
$rez = @('NOLF.rez','NOLF2.rez','NOLFdll.rez','NOLFl.rez','custom',
         'Nolfu003.rez','Nolfcres003.rez','NolfGoty.rez','Modernizer.rez')
$a = @('-windowtitle','TOUR')
foreach ($r in $rez) { $a += @('-rez',$r) }
$a += @('+multiplayer','0')
if ($primary) { $a += @('+CardDesc',$primary.DeviceName) }
$a += @('+RenderDll','d3dstub.ren','+StubNativeFrustum','1','+VRAsymFrustum','0','+VRCrosshair','0',
        '+StubPresentEvery','1','+VRDebugOpenVRPage','2')

$logsDir = Join-Path $Game 'logs'
$before = Get-Date
Write-Host '  launching the tour' -ForegroundColor Cyan
$p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $a -WorkingDirectory $Game -PassThru

$shot = @{}
$done = $false
$t0 = Get-Date
while (-not $done -and ((Get-Date) - $t0).TotalSeconds -lt $Timeout) {
    Start-Sleep -Milliseconds 500
    if ($p.HasExited) { Write-Host '  the game exited during the tour' -ForegroundColor Red; break }
    $log = Get-ChildItem $logsDir -Recurse -Filter client.log -ErrorAction SilentlyContinue |
           Where-Object { $_.LastWriteTime -ge $before } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $log) { continue }
    $txt = Get-Content $log.FullName -Raw -ErrorAction SilentlyContinue
    if (-not $txt) { continue }
    foreach ($m in [regex]::Matches($txt, 'tour page (\d+) of (\d+) \(folder (\d+)\)')) {
        $n = [int]$m.Groups[1].Value
        if ($shot.ContainsKey($n)) { continue }
        Start-Sleep -Seconds 3        # the page settles; the tour moves on at 6
        $png = Join-Path $OutDir ("page{0}-folder{1}.png" -f $n, $m.Groups[3].Value)
        # The whole output, then trimmed: Select-Object -First on the pipe stops
        # window-shot before it saves.
        $ws = @(& (Join-Path $PSScriptRoot 'window-shot.ps1') -Out $png)
        $ws | Where-Object { $_ -match '^(saved|client)' } | ForEach-Object { "    $_" }
        $shot[$n] = $png
        Write-Host ("  page {0} of {1}: {2}" -f $n, $m.Groups[2].Value, $png)
    }
    if ($txt -match 'tour done') { $done = $true }
}

# Only the processes this script started.
if (-not $p.HasExited) { try { $p.CloseMainWindow() | Out-Null } catch {}; Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { try { $p.Kill() } catch {} }
if ($fake -and -not $fake.HasExited) { try { $fake.Kill() } catch {} }
Write-Host ("  {0} pages captured{1}" -f $shot.Count, $(if ($done) { '' } else { ' - the tour did NOT finish' })) `
    -ForegroundColor $(if ($done) { 'Green' } else { 'Red' })
