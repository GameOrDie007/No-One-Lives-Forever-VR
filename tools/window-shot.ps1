# Captures the GAME WINDOW's client area - by handle, not by fixed screen
# coordinates - and says what colour it is.
#
# shot.ps1 grabs a fixed rectangle of the primary display and assumes the game
# is a borderless window at 0,0 at full render size. The renderer stub's window
# is neither: it starts 320x200 wherever Windows put it, and the stub resizes it
# to whatever mode the engine asked for. A fixed rectangle would capture the
# desktop and report it as a result.
#
# So this finds lithtech.exe's own window, moves it to 0,0 so the whole client
# area is on-screen, raises it, captures exactly that rectangle, saves the PNG,
# and prints a colour breakdown. The PNG matters as much as the numbers - four
# wrong answers in this project came from a statistic over pixels nobody opened.
#
# Prints the handle, class, and rectangle it actually captured. If it cannot
# find the window it says so and returns 1 rather than saving something.

param(
    [string]$Out = '',
    [switch]$NoMove,
    # THE SIZE THE GAME WAS ASKED FOR. When Windows virtualises the game's
    # window (the display went to 150% at 05:10 on 9 September and every
    # capture came back 3840x2076 for a 2560x1384 render), the capture is
    # brought back to this size so measurements stay comparable. 0 = as is.
    [int]$Width = 0,
    [int]$Height = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Win {
    // Without this the whole tool lies. The desktop runs at 150% scaling, so a
    // 1024x768 game window reports a 682x512 client rect to a DPI-unaware
    // process while occupying 1024x768 real pixels - and CopyFromScreen works
    // in real pixels. The result is a capture of the top-left two thirds of the
    // frame, which looks exactly like a renderer drawing into the wrong
    // viewport. It cost one confused reading here already.
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    public struct RECT { public int left, top, right, bottom; }
    public struct POINT { public int x, y; }
}
"@

[void][Win]::SetProcessDPIAware()

$proc = Get-Process lithtech -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) {
    Write-Error 'REFUSED: lithtech.exe is not running - there is no window to capture.'
    exit 1
}
$h = $proc.MainWindowHandle
if ($h -eq 0 -or -not [Win]::IsWindowVisible($h)) {
    Write-Error "REFUSED: lithtech.exe (pid $($proc.Id)) has no visible main window."
    exit 1
}

$sb = New-Object System.Text.StringBuilder 256
[void][Win]::GetClassNameW($h, $sb, 256)
$class = $sb.ToString()

if (-not $NoMove) {
    # SWP_NOSIZE | SWP_NOZORDER = 0x0001 | 0x0004
    [void][Win]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, 0, 0, 0x0005)
}
[void][Win]::BringWindowToTop($h)
[void][Win]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 400

$rc = New-Object Win+RECT
[void][Win]::GetClientRect($h, [ref]$rc)
$origin = New-Object Win+POINT
$origin.x = 0; $origin.y = 0
[void][Win]::ClientToScreen($h, [ref]$origin)
$w = $rc.right; $hgt = $rc.bottom

"window : {0:X8}  class '{1}'  pid {2}" -f [int64]$h, $class, $proc.Id
"client : {0}x{1} at screen {2},{3}" -f $w, $hgt, $origin.x, $origin.y
if ($w -le 0 -or $hgt -le 0) { Write-Error 'REFUSED: the client area has no size.'; exit 1 }

$fg = [Win]::GetForegroundWindow()
if ($fg -ne $h) {
    $sb2 = New-Object System.Text.StringBuilder 256
    [void][Win]::GetClassNameW($fg, $sb2, 256)
    "WARNING: the foreground window is '{0}', not the game - the capture may be of something else." -f $sb2.ToString()
}

if (-not $Out) {
    $dir = Join-Path (Split-Path $PSScriptRoot -Parent) 'logs\shots'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $Out = Join-Path $dir ("win-{0:yyyyMMdd-HHmmss}.png" -f (Get-Date))
}

$bmp = New-Object System.Drawing.Bitmap($w, $hgt)
$g = [System.Drawing.Graphics]::FromImage($bmp)
# PrintWindow renders the window itself (PW_CLIENTONLY | PW_RENDERFULLCONTENT),
# so whatever is in front of it on the desktop does not end up in the picture -
# three captures in a row were of another application's window.
$hdc = $g.GetHdc()
$ok = [Win]::PrintWindow($h, $hdc, 3)
$g.ReleaseHdc($hdc)
if (-not $ok) {
    "WARNING: PrintWindow refused; falling back to a screen copy"
    $g.CopyFromScreen($origin.x, $origin.y, 0, 0, (New-Object System.Drawing.Size($w, $hgt)))
}
$g.Dispose()
if ($Width -gt 0 -and $Height -gt 0 -and ($w -ne $Width -or $hgt -ne $Height)) {
    # A virtualised window is LARGER than the render and the render sits in its
    # top-left corner one-to-one, with black beyond (measured 9 September: a
    # 2560x1384 render in a 3840x2076 window). So a larger capture is CROPPED,
    # which keeps every pixel; only a smaller one is scaled up.
    $small = New-Object System.Drawing.Bitmap($Width, $Height)
    $gs = [System.Drawing.Graphics]::FromImage($small)
    if ($w -ge $Width -and $hgt -ge $Height) {
        "cropped: captured {0}x{1}, keeping the top-left {2}x{3} (the window is DPI-virtualised)" -f $w, $hgt, $Width, $Height
        $gs.DrawImage($bmp, (New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)), (New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)), [System.Drawing.GraphicsUnit]::Pixel)
    } else {
        "resized: captured {0}x{1}, saving as {2}x{3}" -f $w, $hgt, $Width, $Height
        $gs.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $gs.DrawImage($bmp, 0, 0, $Width, $Height)
    }
    $gs.Dispose()
    $bmp.Dispose()
    $bmp = $small
    $w = $Width; $hgt = $Height
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)

# Sample a grid rather than every pixel - this runs while the game is live.
$magenta = 0; $cyan = 0; $black = 0; $other = 0; $n = 0
$rs = 0; $gs = 0; $bs = 0
for ($y = 2; $y -lt $hgt; $y += 8) {
    for ($x = 2; $x -lt $w; $x += 8) {
        $p = $bmp.GetPixel($x, $y)
        $rs += $p.R; $gs += $p.G; $bs += $p.B; $n++
        if     ($p.R -gt 200 -and $p.G -lt 60  -and $p.B -gt 200) { $magenta++ }
        elseif ($p.R -lt 60  -and $p.G -gt 200 -and $p.B -gt 200) { $cyan++ }
        elseif ($p.R -lt 24  -and $p.G -lt 24  -and $p.B -lt 24)  { $black++ }
        else { $other++ }
    }
}
$bmp.Dispose()

"saved  : {0}" -f $Out
"pixels : {0} sampled   mean RGB {1},{2},{3}" -f $n, [int]($rs/$n), [int]($gs/$n), [int]($bs/$n)
"        magenta {0:P1}   cyan {1:P1}   black {2:P1}   other {3:P1}" -f ($magenta/$n), ($cyan/$n), ($black/$n), ($other/$n)
if ($magenta/$n -gt 0.9) { "VERDICT: MAGENTA - the device works and the engine stopped calling the renderer." }
elseif ($cyan/$n -gt 0.9) { "VERDICT: CYAN - the engine is driving Clear and SwapBuffers. We are the renderer." }
else { "VERDICT: neither - see the PNG." }
