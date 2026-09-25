# Regression check for the exit crash. No headset needed.
#
# Between July and August 2026 every run this project made ended in an access
# violation: DetourMgr installed a detour on the engine's console print and was
# then leaked to a local that went out of scope, so ~DetourMgr - and the Term()
# that removes the detour - never ran. When CShell.dll unloaded, the engine's
# next console print jumped into freed memory. 186 records in the Windows
# Application log, all at cshell.dll_unloaded+0x6d9b0 = df_Console.
# See docs/EXIT-CRASH.md.
#
# The exit has to be CLEAN for this to test anything. Killing the process never
# unloads the DLL, so a terminated run passes whether the bug is fixed or not.
#
# NOLF's window does NOT act on WM_CLOSE - CloseMainWindow() posts it and the
# game carries on regardless, which is how the first version of this script
# produced a useless "hung on shutdown" for a build that was never asked to shut
# down. The run is ended instead with +VRQuitAfter <seconds>, a client console
# variable that calls g_pLTClient->Shutdown() - the same call the Quit menu item
# makes, and therefore the same teardown path a real player takes.
#
#   .\verify-exit.ps1              one launch, 20 seconds, clean close
#   .\verify-exit.ps1 -Runs 3      three of them

param([int]$Runs = 1, [int]$Seconds = 20, [int]$Tries = 6, [string[]]$Set = @())

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'

function Get-LastCrash {
    $e = Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000} -MaxEvents 50 -ErrorAction SilentlyContinue |
         Where-Object { $_.Message -match 'lithtech' } | Select-Object -First 1
    if ($e) { return $e.TimeCreated }
    return [datetime]::MinValue
}

# Only cshell.dll_unloaded counts as an EXIT crash. Records naming ntdll,
# DINPUT or HID are the startup fault, and during a run with retries there can
# be a dozen of them before the launch that succeeded - an earlier version of
# this script marked the time before the retry loop and so blamed the exit for
# every one of them, reporting "CRASHED ON EXIT (code 0)". Exit code 0 and an
# exit crash cannot both be true; the script was wrong, not the build.
function Show-CrashesSince([datetime]$since) {
    $e = @(Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000} -MaxEvents 50 -ErrorAction SilentlyContinue |
           Where-Object { $_.Message -match 'lithtech' -and $_.TimeCreated -gt $since })
    $exit = 0
    foreach ($x in $e) {
        $m = $x.Message
        $mod = if ($m -match 'Faulting module name: (\S+?),') { $Matches[1] } else { '?' }
        $off = if ($m -match 'Fault offset: (\S+)')           { $Matches[1] } else { '?' }
        $exc = if ($m -match 'Exception code: (\S+)')         { $Matches[1] } else { '?' }
        if ($mod -match 'cshell') {
            $exit++
            Write-Host ("    EXIT    {0}  {1}  {2} @ {3}" -f $x.TimeCreated, $exc, $mod, $off) -ForegroundColor Red
        } else {
            Write-Host ("    startup {0}  {1}  {2} @ {3}" -f $x.TimeCreated, $exc, $mod, $off) -ForegroundColor DarkGray
        }
    }
    return $exit
}

$rez = @('NOLF.rez','NOLF2.rez','NOLFdll.rez','NOLFl.rez','custom',
         'Nolfu003.rez','Nolfcres003.rez','NolfGoty.rez','Modernizer.rez')
$gameArgs = @('-windowtitle','NOLF VR')
foreach ($r in $rez) { $gameArgs += @('-rez',$r) }
$gameArgs += @('+multiplayer','0')
$gameArgs += @('+VRQuitAfter', "$Seconds")
foreach ($s in $Set) {
    $n, $v = $s -split '=', 2
    if (-not $v) { throw "-Set expects Name=Value, got '$s'" }
    $gameArgs += @("+$n", $v)
    Write-Host "console: $n = $v" -ForegroundColor Cyan
}

$clean = 0
$crashed = 0

for ($run = 1; $run -le $Runs; $run++) {
    Write-Host ""
    Write-Host "=== run $run of $Runs ===" -ForegroundColor Cyan

    foreach ($p in @(Get-Process lithtech -ErrorAction SilentlyContinue)) {
        Write-Host "  clearing a leftover lithtech.exe" -ForegroundColor Yellow
        try { $p.Kill() } catch {}
        Start-Sleep -Seconds 2
    }

    $proc = $null
    $mark = Get-LastCrash

    Push-Location $Game
    try {
        for ($i = 1; $i -le $Tries; $i++) {
            $p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $gameArgs -PassThru
            $p.WaitForExit(10000) | Out-Null
            if (-not $p.HasExited) {
                # Alive is not the same as running. The startup fault has two
                # outcomes from one cause: an access violation in ntdll, or a
                # spin at 100% CPU inside RtlFreeHeap under
                # HidD_FreePreparsedData, which survives this wait having never
                # reached the client shell. Responding tells them apart.
                $p.Refresh()
                if (-not $p.Responding) {
                    Write-Host "  alive but not responding - stuck in startup, retry $i" -ForegroundColor Yellow
                    try { $p.Kill() } catch {}
                    Start-Sleep -Seconds 2
                    continue
                }
                # Mark from HERE: everything before this was a failed launch,
                # and its crash records belong to startup, not to the exit.
                $mark = Get-Date
                $proc = $p; break
            }
            Write-Host "  startup crash (exit $($p.ExitCode)) - retry $i" -ForegroundColor Yellow
            Start-Sleep -Seconds 2
        }
    } finally { Pop-Location }

    if (-not $proc) { Write-Host "  never got in after $Tries tries - inconclusive" -ForegroundColor Red; continue }

    Write-Host "  up. it will quit itself after $Seconds s (+VRQuitAfter)"

    if (-not $proc.WaitForExit(($Seconds + 40) * 1000)) {
        Write-Host "  never quit - VRQuitAfter did not fire, or shutdown hung" -ForegroundColor Red
        try { $proc.Kill() } catch {}
        Write-Host "  INCONCLUSIVE: a killed process does not unload the DLL," -ForegroundColor Yellow
        Write-Host "  so this run says nothing about the exit crash." -ForegroundColor Yellow
        continue
    }

    $code = $proc.ExitCode
    Start-Sleep -Seconds 3   # let WER finish writing the event
    $n = Show-CrashesSince $mark

    if ($n -eq 0 -and $code -ne -1073741819) {
        Write-Host "  CLEAN EXIT (code $code), no crash record" -ForegroundColor Green
        $clean++
    } else {
        Write-Host "  CRASHED ON EXIT (code $code, $n new record(s))" -ForegroundColor Red
        $crashed++
    }
}

Write-Host ""
Write-Host "=== $clean clean, $crashed crashed, out of $Runs ===" -ForegroundColor Cyan
if ($crashed -gt 0) { exit 1 }
