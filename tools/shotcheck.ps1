# Launches the game, captures its window, and says whether anything is ON it.
#
# Every instrument this project had looked at the RENDER SURFACE - the field
# calibration reads it back and reported real content while the player was
# looking at a black window. Those are different things: the surface can be
# perfectly drawn and still be presented to the wrong display device.
#
# This looks at the actual window, which is the only thing that matches what a
# person sees.
#
#   .\shotcheck.ps1                 launch, wait, capture
#   .\shotcheck.ps1 -Seconds 25     wait longer (a level load takes ~9 s)
#   .\shotcheck.ps1 -Set VRAutoQuickLoad=1

param([int]$Seconds = 14, [string[]]$Set = @(), [int]$Tries = 6)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'
$Out  = Join-Path $Root 'logs\shotcheck.png'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null

# CopyFromScreen grabs whatever is COMPOSITED at those screen coordinates, not
# the window. On 27 August this tool confidently reported "THE WINDOW HAS AN
# IMAGE ON IT" while capturing a browser sitting on top of the game. An
# instrument that cannot tell the target from whatever is in front of it is
# worse than no instrument, so the window is raised first and the foreground
# window is checked afterwards - if it is not ours, the reading is refused.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class W {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@

# NEVER KILL A GAME THIS SCRIPT DID NOT START. A running lithtech.exe is most
# likely a player's session; stop and say so instead of closing it.
if (Get-Process lithtech -ErrorAction SilentlyContinue) {
    Write-Host '  STOPPED: the game is already running (a player may be in it). Close it first; nothing was touched.' -ForegroundColor Red
    exit 1
}

$rez = @('NOLF.rez','NOLF2.rez','NOLFdll.rez','NOLFl.rez','custom',
         'Nolfu003.rez','Nolfcres003.rez','NolfGoty.rez','Modernizer.rez')
$gameArgs = @('-windowtitle','NOLF VR')
foreach ($r in $rez) { $gameArgs += @('-rez',$r) }
$gameArgs += @('+multiplayer','0')
foreach ($s in $Set) {
    $n, $v = $s -split '=', 2
    if (-not $v) { throw "-Set expects Name=Value, got '$s'" }
    $gameArgs += @("+$n", $v)
    Write-Host "console: $n = $v" -ForegroundColor Cyan
}

$proc = $null
Push-Location $Game
try {
    for ($i = 1; $i -le $Tries; $i++) {
        $p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $gameArgs -PassThru
        $p.WaitForExit(9000) | Out-Null
        if (-not $p.HasExited) { $p.Refresh(); if ($p.Responding) { $proc = $p; break } }
        Write-Host "  launch attempt $i failed - retry" -ForegroundColor Yellow
        try { $p.Kill() } catch {}
        Start-Sleep -Seconds 2
    }
} finally { Pop-Location }

if (-not $proc) { throw 'never got in' }

Write-Host "up. settling for $Seconds s ..."
Start-Sleep -Seconds $Seconds
$proc.Refresh()

$h = $proc.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { try { $proc.Kill() } catch {}; throw 'no main window' }

[void][W]::ShowWindow($h, 5)          # SW_SHOW
[void][W]::BringWindowToTop($h)
[void][W]::SetForegroundWindow($h)
Start-Sleep -Seconds 2

$fg = [W]::GetForegroundWindow()
if ($fg -ne $h) {
    Write-Host ("REFUSING TO MEASURE: the game window is not in front (foreground={0}, game={1})." -f $fg, $h) -ForegroundColor Red
    Write-Host 'A capture taken now would be of whatever is on top, not of the game.' -ForegroundColor Red
    try { $proc.Kill() } catch {}
    exit 2
}

$r = New-Object W+RECT
[void][W]::GetWindowRect($h, [ref]$r)
$w = $r.R - $r.L
$ht = $r.B - $r.T
Write-Host ("window at ({0},{1}) size {2}x{3}  visible={4}" -f $r.L, $r.T, $w, $ht, [W]::IsWindowVisible($h))

if ($w -le 0 -or $ht -le 0) { try { $proc.Kill() } catch {}; throw 'degenerate window rect' }

$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g   = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)

# Sample a grid and report mean and spread. A black window is not just dark, it
# is FLAT - sd near zero is the thing that distinguishes it from a dim scene.
$sum = 0.0; $sumSq = 0.0; $n = 0; $nonBlack = 0
for ($y = 0; $y -lt $ht; $y += [Math]::Max(1, [int]($ht / 60))) {
    for ($x = 0; $x -lt $w; $x += [Math]::Max(1, [int]($w / 60))) {
        $c = $bmp.GetPixel($x, $y)
        $l = 0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B
        $sum += $l; $sumSq += $l * $l; $n++
        if ($l -gt 8) { $nonBlack++ }
    }
}
$bmp.Dispose()
try { $proc.Kill() } catch {}

$mean = $sum / $n
$sd   = [Math]::Sqrt([Math]::Max(0, $sumSq / $n - $mean * $mean))
Write-Host ""
Write-Host ("samples {0}   mean luminance {1:N1}   sd {2:N1}   non-black {3:P0}" -f $n, $mean, $sd, ($nonBlack / $n))
if ($sd -lt 3 -and $mean -lt 12) {
    Write-Host 'THE WINDOW IS BLACK - flat and dark, nothing is being presented to it.' -ForegroundColor Red
} else {
    Write-Host 'THE WINDOW HAS AN IMAGE ON IT.' -ForegroundColor Green
}
Write-Host "saved $Out"
