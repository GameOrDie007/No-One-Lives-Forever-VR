# Saves what the game window is showing to a PNG, so a desk run produces
# something that can be LOOKED AT rather than only counted.
#
# Four wrong answers in this project's history came from correlating a statistic
# over pixels nobody had opened. The eye-stash correlation reported 0.038 while
# reading an all-black surface; the field sweep reported a confident tan ratio
# from a scene that turned out to be a blank wall. Both would have been caught
# in one glance.
#
# Captures the primary display region the borderless window occupies. If the
# window is not at (0,0) at full render size, pass -W/-H/-X/-Y.

param(
    [string]$Out = '',
    [int]$X = 0, [int]$Y = 0, [int]$W = 2880, [int]$H = 1494,
    [int]$Scale = 3,
    [switch]$AnyWindow
)

Add-Type -AssemblyName System.Drawing

# CopyFromScreen grabs whatever is COMPOSITED at those coordinates, not the
# window. The first run of the vertical-field measurement captured the editor
# that happened to be in front, and 64 KB of dark UI would have gone into a
# correlation as if it were a game frame. shotcheck.ps1 already carries this
# warning from 27 August; shot.ps1 did not, and paid for it the same way.
#
# So: refuse unless the game is actually in front. -AnyWindow overrides, for
# capturing something that is deliberately not the game.
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Fg {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
}
"@ -ErrorAction SilentlyContinue

if (-not $AnyWindow) {
    $ok = $false
    for ($i = 0; $i -lt 20; $i++) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][Fg]::GetClassNameW([Fg]::GetForegroundWindow(), $sb, 256)
        if ($sb.ToString() -eq 'LithTech') { $ok = $true; break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $ok) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][Fg]::GetClassNameW([Fg]::GetForegroundWindow(), $sb, 256)
        Write-Error ("REFUSED: the foreground window is '{0}', not the game. " -f $sb.ToString() +
                     "A capture of whatever happens to be in front is not a measurement.")
        exit 1
    }
}

if (-not $Out) {
    $dir = Join-Path (Split-Path $PSScriptRoot -Parent) 'logs\shots'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $Out = Join-Path $dir ("shot-{0:yyyyMMdd-HHmmss}.png" -f (Get-Date))
}

$full = New-Object System.Drawing.Bitmap($W, $H)
$g = [System.Drawing.Graphics]::FromImage($full)
$g.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size($W, $H)))
$g.Dispose()

if ($Scale -gt 1) {
    $sw = [int]($W / $Scale); $sh = [int]($H / $Scale)
    $small = New-Object System.Drawing.Bitmap($sw, $sh)
    $gs = [System.Drawing.Graphics]::FromImage($small)
    $gs.InterpolationMode = 'HighQualityBicubic'
    $gs.DrawImage($full, 0, 0, $sw, $sh)
    $gs.Dispose()
    $small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose()
    "{0}  ({1}x{2}, downscaled {3}x from {4}x{5})" -f $Out, $sw, $sh, $Scale, $W, $H
} else {
    $full.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    "{0}  ({1}x{2})" -f $Out, $W, $H
}
$full.Dispose()
