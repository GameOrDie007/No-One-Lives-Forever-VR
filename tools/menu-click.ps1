# Drive the MAIN MENU from the desk, with nobody in the room.
#
# The 5 September crash happens when Single Player is selected, which no capture
# harness could reach: look-shot photographs the menu and quits, and +runworld
# loads a level before the menu exists. So neither of them has ever executed the
# folder change, which is where the crash is.
#
# The menu is keyboard-navigable, so it can be driven. Launch, wait for the menu
# to settle, focus the window, send the keys, wait, and then say whether the
# renderer's log records a CRASH block. That turns a headset round into a
# twenty-second desk run.
#
#   .\tools\menu-click.ps1                       # Enter (Single player)
#   .\tools\menu-click.ps1 -Keys '{DOWN}{ENTER}' # Continue game
#   .\tools\menu-click.ps1 -Set @('StubTexDump=1')

param(
    # AN ARRAY, SENT ONE STEP AT A TIME. SendKeys delivers a whole string in
    # one burst and the menu drops everything after the first key, so driving
    # more than one folder deep needs a pause between the steps:
    #   -Keys '{ENTER}','{ENTER}'   Single player, then New game
    [string[]]$Keys  = @('{ENTER}'),
    [int]     $StepDelay = 6,      # seconds between the steps
    [int]     $Settle = 16,        # seconds before the keys
    [int]     $After  = 12,        # seconds after them, to let it fall over
    [string[]]$Set   = @(),
    [string]  $Out   = 'logs\menuclick',
    # d3d.ren answers the only question that matters first: is OUR renderer
    # in this at all?
    [string]  $Renderer = 'd3dstub.ren'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'
# NEVER KILL A GAME THIS SCRIPT DID NOT START. A running lithtech.exe is most
# likely a player's session; stop and say so instead of closing it.
if (Get-Process lithtech -ErrorAction SilentlyContinue) {
    Write-Host '  STOPPED: the game is already running (a player may be in it). Close it first; nothing was touched.' -ForegroundColor Red
    exit 1
}
$OutDir = Join-Path $Root $Out
New-Item -ItemType Directory -Force $OutDir | Out-Null

Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Fg {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
'@

# Only a leftover fake host, never every python on the machine.
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'fakehost' } |
    ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force } catch {} }
Start-Sleep -Milliseconds 800

$fakeLog = Join-Path $Root 'logs\fakehost-click.log'
New-Item -ItemType Directory -Force (Split-Path $fakeLog) | Out-Null
$fake = Start-Process -FilePath 'python' `
        -ArgumentList @(('"' + (Join-Path $PSScriptRoot 'fakehost.py') + '"'), '--static', '0,0,0') `
        -PassThru -WindowStyle Minimized -RedirectStandardOutput $fakeLog
Start-Sleep -Seconds 2
if ($fake.HasExited) { throw "fake host exited (exit $($fake.ExitCode))" }

$live = Join-Path $Game 'logs\renstub.log'
if (Test-Path $live) { Remove-Item $live -Force -ErrorAction SilentlyContinue }

$primary = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Primary } | Select-Object -First 1
$rez = @('NOLF.rez','NOLF2.rez','NOLFdll.rez','NOLFl.rez','custom',
         'Nolfu003.rez','Nolfcres003.rez','NolfGoty.rez','Modernizer.rez')
$a = @('-windowtitle','CLICK')
foreach ($r in $rez) { $a += @('-rez',$r) }
$a += @('+multiplayer','0')
if ($primary) { $a += @('+CardDesc',$primary.DeviceName) }
$a += @('+RenderDll', $Renderer)
if ($Renderer -eq 'd3dstub.ren') {
    $a += @('+StubNativeFrustum','1','+VRAsymFrustum','0','+VRCrosshair','0')
}
foreach ($e in $Set) { $n,$v = $e -split '=',2; $a += @("+$n", $v) }

Write-Host ("  launching, keys '{0}' after {1}s" -f ($Keys -join ' '), $Settle) -ForegroundColor Cyan
$p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $a `
                   -WorkingDirectory $Game -PassThru

for ($s = 0; $s -lt $Settle; $s++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { Write-Host "  exited before the keys, after $s s" -ForegroundColor Red; break }
}

$died = $false
if (-not $p.HasExited) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
        [Fg]::ShowWindow($p.MainWindowHandle, 9) | Out-Null    # SW_RESTORE
        [Fg]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
        Start-Sleep -Milliseconds 700
    }
    for ($k = 0; $k -lt $Keys.Count; $k++) {
        if ($p.HasExited) { $died = $true; Write-Host "  DIED before step $($k+1)" -ForegroundColor Red; break }
        # EVERY key, not just the first: focus can move mid-sequence (a click,
        # a notification), and a key sent then lands in someone else's window.
        if ([Fg]::GetForegroundWindow() -ne $p.MainWindowHandle) {
            Write-Host ("  STOPPED before step {0}: the game is not in front; the rest NOT sent" -f ($k+1)) -ForegroundColor Red
            break
        }
        Write-Host ("  sending step {0}/{1}: {2}" -f ($k+1), $Keys.Count, $Keys[$k]) -ForegroundColor Cyan
        [System.Windows.Forms.SendKeys]::SendWait($Keys[$k])
        if ($k -lt $Keys.Count - 1) {
            for ($w = 0; $w -lt $StepDelay; $w++) {
                Start-Sleep -Seconds 1
                if ($p.HasExited) { $died = $true; Write-Host "  DIED $w s after step $($k+1)" -ForegroundColor Red; break }
            }
            if ($died) { break }
        }
    }
    for ($s = 0; $s -lt $After -and -not $died; $s++) {
        Start-Sleep -Seconds 1
        if ($p.HasExited) { $died = $true; Write-Host "  DIED $s s after the keys" -ForegroundColor Red; break }
    }
}

if (-not $p.HasExited) { try { $p.CloseMainWindow() | Out-Null } catch {} ; Start-Sleep -Seconds 2 }
# Only the process this script started.
if (-not $p.HasExited) { try { $p.Kill() } catch {} }
if ($fake -and -not $fake.HasExited) { try { $fake.Kill() } catch {} }
Start-Sleep -Milliseconds 500

$dst = Join-Path $OutDir 'renstub.log'
# DELETE THE DESTINATION FIRST. With +RenderDll d3d.ren ours never loads and
# never writes a log, so the copy below copies nothing and the verdict was
# read off the PREVIOUS run's file - which reported a crash for an arm that
# had not crashed, with the previous arm's registers in it.
if (Test-Path $dst) { Remove-Item $dst -Force -ErrorAction SilentlyContinue }
if (Test-Path $live) { Copy-Item $live $dst -Force -ErrorAction SilentlyContinue }

# THE PROCESS DYING IS THE PRIMARY SIGNAL, because it is true for every
# renderer. The log block is corroboration and only ours writes one.
$verdict = if ($died) { 'CRASHED (process gone)' } else { 'survived' }
if (Test-Path $dst) {
    $txt = Get-Content $dst -Raw
    if ($txt -match '=== CRASH ===') {
        $verdict = 'CRASHED (crash block logged)'
        ($txt -split "`n" | Select-String -Pattern '=== CRASH ===' -Context 0,6).ToString() |
            ForEach-Object { Write-Host $_ -ForegroundColor Red }
    }
}
Write-Host ("  VERDICT: {0}" -f $verdict) `
    -ForegroundColor $(if ($verdict -like 'CRASHED*') { 'Red' } else { 'Green' })
