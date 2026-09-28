# ---------------------------------------------------------------------------
# menu-mouse.ps1 - CLICK a menu item with the MOUSE, at the desk, and see what
# page comes up.
#
# Keyboard driving (menu-click.ps1, look-shot -Keys) proves a folder change
# but says nothing about mouse HIT-TESTING, and hit-testing is exactly what a
# font-size change can break: CLTGUITextItemCtrl's rectangle comes from the
# font's own extents (CalculateSize), so if a scaled sheet is drawn but the
# control still measures the old one, the words are big and the clickable
# area is small. Only a click at the drawn position settles it.
#
#   .\tools\menu-mouse.ps1 -At "260,1132" -Expect "options"
#
# -At is in REAL screen pixels of the left eye (the game is borderless at 0,0).
# -Expect is a word the target page's client log or capture should show; the
# verdict is whether lithtech is still alive and whether the capture changed,
# plus the log line the folder switch writes.
# ---------------------------------------------------------------------------
param(
    [Parameter(Mandatory = $true)][string]$At,     # "x,y" real pixels
    [int]   $ResW   = 3840,
    [int]   $ResH   = 2160,
    [int]   $Settle = 26,
    [int]   $After  = 6,
    [string]$Out    = 'logs\menu-mouse.png',
    [string[]]$Set  = @(),
    # MONO, no fake host - the only mode in which the pixel where an item is
    # DRAWN is where the client BELIEVES it is. In stereo the renderer fits the
    # whole layout into each half, so a click at the drawn position lands
    # somewhere else in client space and the test measures the fit, not the
    # hit rects. A stereo click that misses proves nothing about fonts.
    [switch]$Mono
)

$ErrorActionPreference = 'Continue'
$Root = Split-Path $PSScriptRoot -Parent
# NEVER KILL A GAME THIS SCRIPT DID NOT START. A running lithtech.exe is most
# likely a player's session; stop and say so instead of closing it.
if (Get-Process lithtech -ErrorAction SilentlyContinue) {
    Write-Host '  STOPPED: the game is already running (a player may be in it). Close it first; nothing was touched.' -ForegroundColor Red
    exit 1
}
$x, $y = ($At -split ',') | ForEach-Object { [int]$_ }

Add-Type -Namespace MM -Name In -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint data, UIntPtr extra);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
'@
$null = [MM.In]::SetProcessDPIAware()

# Launch through look-shot's launcher path but without its capture: run.ps1
# in the background, menu mode, fake host, same settings the menu captures use.
$fake = $null
if (-not $Mono) {
    $fakeLog = Join-Path $Root 'logs\fakehost-mouse.log'
    $fake = Start-Process -FilePath 'python' -ArgumentList @(('"' + (Join-Path $PSScriptRoot 'fakehost.py') + '"'), '--static', '0,0,0') `
            -PassThru -WindowStyle Minimized -RedirectStandardOutput $fakeLog
    Start-Sleep -Seconds 2
}
# StubPresentEvery=1 is what makes the desk window present at all; without it
# the capture is black and the click lands on nothing. look-shot adds it too.
# Mono is the compare-ours.ps1 recipe: one view, no per-eye centring.
$stereo = if ($Mono) { 'VRStereo=0' } else { 'VRStereo=2' }
$native = if ($Mono) { 'StubNativeFrustum=0' } else { 'StubNativeFrustum=1' }
$setAll = @($native,'VRAsymFrustum=0','VRCrosshair=0',$stereo,'soundenable=0','musicenable=0','StubPresentEvery=1') + $Set
$runner = Start-Process -FilePath 'powershell' -PassThru -ArgumentList @(
    '-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'run.ps1'),
    '-RenderDll','d3dstub.ren','-Title','MOUSE','-Width',"$ResW",'-Height',"$ResH",
    '-Set',($setAll -join ','))

# THE GAME THIS SCRIPT STARTED is run.ps1's child, found by parent pid and
# never by name: any other lithtech.exe may be a player's.
function Get-OwnGame($runner) {
    $c = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($runner.Id) AND Name='lithtech.exe'" -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($c) { try { return Get-Process -Id $c.ProcessId -ErrorAction Stop } catch {} }
    return $null
}

Write-Host ("  waiting {0} s for the menu..." -f $Settle) -ForegroundColor DarkGray
Start-Sleep -Seconds $Settle
$g = Get-OwnGame $runner
if (-not $g -or $g.HasExited) {
    Write-Host '  the game is not running' -ForegroundColor Red
    if ($fake -and -not $fake.HasExited) { try { $fake.Kill() } catch {} }
    if ($runner -and -not $runner.HasExited) { try { $runner.Kill() } catch {} }
    exit 1
}

# BEFORE the click, so a miss still shows where the items actually are - the
# first run at any new resolution is as much a survey as a test.
$before = (Join-Path $Root $Out) -replace '\.png$', '-before.png'
& (Join-Path $PSScriptRoot 'window-shot.ps1') -Out $before -NoMove -Width $ResW -Height $ResH | Out-Null
Write-Host ("  before-click capture: {0}" -f $before) -ForegroundColor DarkGray

# Bring it to the front, and REFUSE to click if it is not there - a click that
# lands on some other window is worse than no click.
$null = [MM.In]::SetForegroundWindow($g.MainWindowHandle)
Start-Sleep -Milliseconds 600
if ([MM.In]::GetForegroundWindow() -ne $g.MainWindowHandle) {
    Write-Host '  REFUSED: the game window is not in front; not clicking' -ForegroundColor Red
} else {
    # Move first, let the menu notice the hover, then press and release.
    $null = [MM.In]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 400
    # Again at the moment of the press: focus can move during the hover wait.
    if ([MM.In]::GetForegroundWindow() -ne $g.MainWindowHandle) {
        Write-Host '  STOPPED: the game is no longer in front; not clicking' -ForegroundColor Red
    } else {
        [MM.In]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
        Start-Sleep -Milliseconds 80
        [MM.In]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
        Write-Host ("  clicked at {0},{1}" -f $x, $y) -ForegroundColor Cyan
    }
}
Start-Sleep -Seconds $After

$alive = -not $g.HasExited
& (Join-Path $PSScriptRoot 'window-shot.ps1') -Out (Join-Path $Root $Out) -NoMove -Width $ResW -Height $ResH |
    ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkGray }

# Only the process this script started.
if (-not $g.HasExited) { try { $g.CloseMainWindow() | Out-Null } catch {} }
Start-Sleep -Seconds 2
if (-not $g.HasExited) { try { $g.Kill() } catch {} }
if ($fake -and -not $fake.HasExited) { try { $fake.Kill() } catch {} }
if ($runner -and -not $runner.HasExited) { try { $runner.Kill() } catch {} }

# What page did the click land on? The client says when a folder changes.
$d = Get-ChildItem (Join-Path $Root 'game\logs') -Directory | Where-Object { $_.Name -match '^\d{8}-\d{6}$' } |
     Sort-Object Name -Descending | Select-Object -First 1
if ($d) {
    $sw = Select-String -Path (Join-Path $d.FullName 'client.log') -Pattern 'SwitchToFolder|folder|FOLDER' -Encoding ascii |
          Select-Object -Last 4
    Write-Host '  last folder lines in the client log:' -ForegroundColor Cyan
    $sw | ForEach-Object { Write-Host ('    ' + $_.Line.Trim()) }
}
Write-Host ("  game alive after the click: {0}" -f $alive) -ForegroundColor $(if ($alive) { 'Green' } else { 'Red' })
