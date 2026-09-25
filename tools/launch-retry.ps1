# Clears hung lithtech.exe processes and launches the game, retrying if it
# crashes on startup.
#
# As of 23 August 2026 the game crashes with an access violation on roughly
# three launches in four, on this machine, BEFORE any of our code runs - the
# stock game with no Modernizer.rez and no VR client does exactly the same. See
# docs/STARTUP-CRASH.md. This is a workaround, not a fix.
#
# It also clears leftovers first: a crashed instance sometimes stays in the
# process list not responding, and while one of those is alive every subsequent
# launch fails, which makes an intermittent fault look like a permanent one.

param([int]$Tries = 8, [string[]]$Set = @())

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'

function Clear-Zombies {
    $procs = @(Get-Process lithtech -ErrorAction SilentlyContinue)
    if ($procs.Count) {
        Write-Host "clearing $($procs.Count) leftover lithtech.exe process(es)" -ForegroundColor Yellow
        foreach ($p in $procs) { try { $p.Kill() } catch {} }
        Start-Sleep -Seconds 3
    }
}

$rez = @(
    'NOLF.rez', 'NOLF2.rez', 'NOLFdll.rez', 'NOLFl.rez', 'custom',
    'Nolfu003.rez', 'Nolfcres003.rez', 'NolfGoty.rez',
    'Modernizer.rez'
)
$gameArgs = @('-windowtitle', 'NOLF VR')
foreach ($r in $rez) { $gameArgs += @('-rez', $r) }
$gameArgs += @('+multiplayer', '0')
foreach ($s in $Set) {
    $name, $value = $s -split '=', 2
    if (-not $value) { throw "-Set expects Name=Value, got '$s'" }
    $gameArgs += @("+$name", $value)
    Write-Host "console: $name = $value" -ForegroundColor Cyan
}

Clear-Zombies

Push-Location $Game
try {
    for ($i = 1; $i -le $Tries; $i++) {
        Write-Host "launch attempt $i of $Tries ..." -ForegroundColor Cyan
        $p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $gameArgs -PassThru

        # A startup crash happens within a few seconds. Anything still alive
        # after that has got past the renderer and is really running.
        $p.WaitForExit(10000) | Out-Null

        if (-not $p.HasExited) {
            Write-Host "up and running - play as normal, this window can be ignored." -ForegroundColor Green
            $p.WaitForExit()
            Write-Host "game exited (code $($p.ExitCode))."
            return
        }

        if ($p.ExitCode -eq -1073741819) {
            Write-Host "  crashed on startup (access violation) - retrying" -ForegroundColor Yellow
            Clear-Zombies
            Start-Sleep -Seconds 2
        } else {
            Write-Host "game exited with code $($p.ExitCode) - not the startup crash, stopping." -ForegroundColor Yellow
            return
        }
    }
    Write-Host "gave up after $Tries attempts." -ForegroundColor Red
    Write-Host "Try closing Virtual Desktop (Streamer AND Service) and running this again." -ForegroundColor Yellow
}
finally { Pop-Location }
