# ---------------------------------------------------------------------------
# auto-tour.ps1 - photograph every single-player level from inside its rooms.
#
# The 103-world sweep proves a level LOADS. It spends eight seconds at the
# spawn point and answers nothing about the other thirty rooms. This stands in
# eight places per level - the level's own AI nodes and teleport points, spread
# by tools/tour-points.py - and photographs each one, in god mode so nothing
# interrupts it, with the whole client and renderer log kept per level.
#
# What it catches, unattended: a crash or a stall placed at a level and a
# moment; per-level counts of untextured polygons, missing pictures, effects,
# lights and sky layers, which stand out against the other sixty; and a few
# hundred pictures a person can flick through for anything black, white,
# missing or floating.
#
# What it CANNOT catch, and why a headset tour is still needed: whether a
# thing is the right SIZE, in the right PLACE, or flickering; anything about
# stereo, scale, hands or comfort. A picture of a scene is not the scene.
#
#   .\tools\auto-tour.ps1                      every single-player world
#   .\tools\auto-tour.ps1 -Only M08            one mission
#   .\tools\auto-tour.ps1 -Stops 12 -EverySec 8
#
# Roughly a minute a level: 61 worlds is about an hour.
# ---------------------------------------------------------------------------
param(
    [string]$List     = 'tools\worlds.txt',
    [string]$Only     = '',
    [int]   $Start    = 0,
    [int]   $Count    = 0,
    [int]   $Stops    = 8,
    [double]$EverySec = 6.0,
    [int]   $LoadSec  = 12,          # world up and settled before the first stop
    [string]$OutDir   = 'logs\tour',
    [switch]$IncludeMulti,
    [int]   $ResW     = 2560,
    [int]   $ResH     = 1384,
    [string[]]$Set    = @()
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

# THE PLAYER'S SETTINGS ARE NOT THE DESK'S - the engine writes autoexec.cfg
# when the game closes. Snapshot before, restore after; a snapshot a killed run
# left behind is restored first. See look-shot.ps1.
$cfgLive = Join-Path $Game 'autoexec.cfg'
$cfgSnap = Join-Path $Root 'logs\autoexec.headset.cfg'
if (Test-Path $cfgSnap) { Copy-Item $cfgSnap $cfgLive -Force }
elseif (Test-Path $cfgLive) { Copy-Item $cfgLive $cfgSnap -Force }

$LogDir = Join-Path $Game 'logs'
$Out    = Join-Path $Root $OutDir
New-Item -ItemType Directory -Force $Out | Out-Null

$worlds = Get-Content (Join-Path $Root $List) | Where-Object { $_.Trim() }
if (-not $IncludeMulti) {
    $worlds = $worlds | Where-Object { $_ -notmatch 'MULTI' -and $_ -notmatch 'EXTRAS' }
}
if ($Only)         { $worlds = $worlds | Where-Object { $_ -match [regex]::Escape($Only) } }
if ($Start -gt 0)  { $worlds = $worlds | Select-Object -Skip $Start }
if ($Count -gt 0)  { $worlds = $worlds | Select-Object -First $Count }

$perLevel = $LoadSec + [int]($Stops * $EverySec) + 6
Write-Host ("touring {0} worlds, {1} stops each, ~{2} s per level (~{3} min total)" -f `
            $worlds.Count, $Stops, $perLevel, [math]::Round($worlds.Count * $perLevel / 60.0)) -ForegroundColor Cyan

# ONE fake host for the whole run. Hands active so the view weapon is drawn and
# the aim marker has something to follow; no buttons, because a tour that fires
# fails missions and empties magazines.
$fakeLog = Join-Path $Root 'logs\fakehost-tour.log'
New-Item -ItemType Directory -Force (Split-Path $fakeLog) | Out-Null
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'fakehost' } |
    ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force } catch {} }
$fake = Start-Process -FilePath 'python' `
        -ArgumentList @(('"' + (Join-Path $PSScriptRoot 'fakehost.py') + '"'), '--rhand', '0,0,0') `
        -PassThru -WindowStyle Minimized -RedirectStandardOutput $fakeLog
Start-Sleep -Seconds 2
if ($fake.HasExited) { throw "fake host exited immediately (exit $($fake.ExitCode))" }

Add-Type -AssemblyName System.Windows.Forms
$primary = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Primary } | Select-Object -First 1

# BRING THE GAME TO THE FRONT, OR THE PICTURES ARE BLACK.
#
# PrintWindow on a D3D11 swapchain returns a black rectangle when the window is
# not composited to the front, and it returns TRUE while doing it - so the first
# tour ran perfectly, wrote six 2560x1384 pictures, and every one was empty
# while the log showed 4000 presents. SetForegroundWindow is refused to a
# process that is not itself in front, so this asks a few times.
#
# NO ALT TAP. It used to send one to win the foreground, but that key goes to
# whatever window IS in front - by definition not the game - and a person
# working in it gets their menu bar opened under them. A key is sent only to
# the game; a window that will not come forward costs blank pictures, which
# the blank check below counts.
Add-Type -Name Fg -Namespace TourW -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
'@
function Bring-Front($proc) {
    $proc.Refresh()
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { return $false }
    for ($try = 0; $try -lt 6; $try++) {
        [TourW.Fg]::ShowWindow($proc.MainWindowHandle, 9) | Out-Null
        [TourW.Fg]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
        Start-Sleep -Milliseconds 500
        if ([TourW.Fg]::GetForegroundWindow() -eq $proc.MainWindowHandle) { return $true }
    }
    return $false
}
# Is a picture empty? A capture that failed is uniformly black; a dark ROOM is
# not, so this asks for near-zero across a grid rather than a low average.
function Test-Blank($path) {
    try {
        $bmp = [System.Drawing.Bitmap]::FromFile($path)
        $lit = 0
        for ($y = 8; $y -lt $bmp.Height; $y += 64) {
            for ($x = 8; $x -lt $bmp.Width; $x += 64) {
                $c = $bmp.GetPixel($x, $y)
                if ($c.R -gt 12 -or $c.G -gt 12 -or $c.B -gt 12) { $lit++ }
            }
        }
        $bmp.Dispose()
        return ($lit -lt 4)
    } catch { return $false }
}

$rez = @('NOLF.rez','NOLF2.rez','NOLFdll.rez','NOLFl.rez','custom',
         'Nolfu003.rez','Nolfcres003.rez','NolfGoty.rez','Modernizer.rez')

$rows = @()
$csv  = Join-Path $Out 'summary.csv'
$i = 0
$p = $null
$ErrorActionPreference = 'Continue'

foreach ($w in $worlds) {
    $i++
  try {
    # Any lithtech.exe but the previous level's own is one this script did
    # not start - someone launched the game mid-run. Leave it alone.
    if (Get-Process lithtech -ErrorAction SilentlyContinue | Where-Object { -not $p -or $_.Id -ne $p.Id }) {
        Write-Host '  STOPPED: another lithtech.exe is running (a player may be in it). It was left alone; the tour ends here.' -ForegroundColor Red
        break
    }
    $short = ($w -replace '\.DAT$','') -replace '/','\'
    $tag   = ($w -replace '\.DAT$','') -replace '[/\\]','_'
    Write-Host ("[{0}/{1}] {2}" -f $i, $worlds.Count, $short) -ForegroundColor Yellow

    $dst = Join-Path $Out $tag
    New-Item -ItemType Directory -Force $dst | Out-Null

    # The stops, from the level's own furniture.
    $tourFile = Join-Path $Game 'vrtour.txt'
    $ptOut = & python (Join-Path $PSScriptRoot 'tour-points.py') $w $Stops $tourFile 2>&1
    $nStops = 0
    if (Test-Path $tourFile) {
        $nStops = (Get-Content $tourFile | Where-Object { $_ -and $_ -notmatch '^#' }).Count
        Copy-Item $tourFile (Join-Path $dst 'vrtour.txt') -Force -ErrorAction SilentlyContinue
    }
    Write-Host ("      {0} stops" -f $nStops) -ForegroundColor DarkGray

    $live = Join-Path $LogDir 'renstub.log'
    if (Test-Path $live) { Remove-Item $live -Force -ErrorAction SilentlyContinue }

    $gameArgs = @('-windowtitle','NOLF VR')
    foreach ($r in $rez) { $gameArgs += @('-rez',$r) }
    $gameArgs += @('+multiplayer','0')
    if ($primary) { $gameArgs += @('+CardDesc',$primary.DeviceName) }
    $gameArgs += @('+RenderDll','d3dstub.ren','+StubNativeFrustum','1',
                   '+VRAsymFrustum','0','+VRCrosshair','0','+VRStereo','2')
    $gameArgs += @('+ScreenWidth',"$ResW",'+ScreenHeight',"$ResH")
    # THE DESK STILL PRESENTS. With a host live the renderer stops painting the
    # desktop window - the headset is the only picture that matters in play -
    # so a capture of that window is whatever was last left in it. The first
    # tour wrote six black pictures for exactly this reason, with the window in
    # front and PrintWindow returning success. This harness reads the window,
    # so it asks for every frame. Same line, same reason, as look-shot.ps1.
    $gameArgs += @('+StubPresentEvery','1')
    # Silent by default - sixty levels of overlapping audio at 3 a.m. helps
    # nobody - but -Set soundenable=1 has to win, or the one run that needs
    # sound to answer a question cannot ask for it.
    if (-not ($Set | Where-Object { $_ -match 'soundenable' })) {
        $gameArgs += @('+soundenable','0','+musicenable','0')
    }
    $gameArgs += @('+runworld', ('"' + $short + '"'))
    $gameArgs += @('+VRTour','1','+VRTourEvery',"$EverySec",'+VRCheats','7')
    foreach ($e in $Set) {
        foreach ($kv in ($e -split '[;,]')) {
            if ($kv.Trim()) { $n,$v = $kv -split '=',2; $gameArgs += @("+$n", $v) }
        }
    }

    $t0 = Get-Date
    $env:__COMPAT_LAYER = 'HIGHDPIAWARE'
    $env:SDL_WINDOWS_DPI_AWARENESS = 'system'
    $p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $gameArgs `
                       -WorkingDirectory $Game -PassThru

    # Let it load, then photograph each stop a moment after the client hops.
    $shots = 0
    $died  = $false
    $blanks = 0
    Start-Sleep -Seconds $LoadSec
    if ($p.HasExited) { $died = $true }
    if (-not $died) {
        if (-not (Bring-Front $p)) {
            Write-Host '      the window would not come to the front - pictures may be black' -ForegroundColor Red
        }
        # TWO PICTURES PER STOP, and they are not the same picture. The fake
        # host sweeps the head +-40 degrees on a 7.6 second period, so two
        # captures three seconds apart look in noticeably different
        # directions - which is most of what a second stop would have bought,
        # for none of the loading.
        foreach ($s in 1..([math]::Max($nStops, 1))) {
          foreach ($half in @('a','b')) {
            Start-Sleep -Milliseconds ([int](($EverySec * 1000) * 0.35))
            if ($p.HasExited) { $died = $true; break }
            $shot = Join-Path $dst ("stop{0:d2}{1}.png" -f $s, $half)
            try {
                & (Join-Path $PSScriptRoot 'window-shot.ps1') -Out $shot -Width $ResW -Height $ResH |
                    Out-Null
                # A blank capture means the window slipped behind something.
                # Take it again once, from the front.
                if ((Test-Path $shot) -and (Test-Blank $shot)) {
                    Bring-Front $p | Out-Null
                    Start-Sleep -Milliseconds 400
                    & (Join-Path $PSScriptRoot 'window-shot.ps1') -Out $shot -Width $ResW -Height $ResH |
                        Out-Null
                    if (Test-Blank $shot) { $blanks++ }
                }
                if (Test-Path $shot) { $shots++ }
            } catch {}
          }
          if ($died) { break }
        }
    }

    $secs = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $exitCode = ''
    if ($p.HasExited) { $exitCode = $p.ExitCode }
    else {
        try { $p.CloseMainWindow() | Out-Null } catch {}
        Start-Sleep -Seconds 2
    }
    # Only the process this script started.
    if (-not $p.HasExited) { try { $p.Kill() } catch {}; $p.WaitForExit(5000) | Out-Null }
    Start-Sleep -Milliseconds 800

    # The logs, from this run's own directory.
    $run = Get-ChildItem $LogDir -Directory -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    foreach ($f in @('renstub.log','client.log')) {
        $src = $null
        if ($run -and (Test-Path (Join-Path $run.FullName $f))) { $src = Join-Path $run.FullName $f }
        elseif (Test-Path (Join-Path $LogDir $f)) { $src = Join-Path $LogDir $f }
        if ($src) { Copy-Item $src (Join-Path $dst $f) -Force -ErrorAction SilentlyContinue }
    }

    # What the client says it did.
    $visited = 0
    $complete = $false
    $pushed = 0
    $cl = Join-Path $dst 'client.log'
    if (Test-Path $cl) {
        $txt = Get-Content $cl -Raw -ErrorAction SilentlyContinue
        if ($txt) {
            $visited  = ([regex]::Matches($txt, 'VRTour: stop \d+ of')).Count
            $pushed   = ([regex]::Matches($txt, 'MOVED BY THE WORLD')).Count
            $complete = $txt -match 'VRTour: COMPLETE'
        }
    }

    $note = ''
    if ($died -and $exitCode -ne 0 -and $exitCode -ne '') { $note = "GAME EXITED (exit $exitCode)" }
    elseif ($died) { $note = 'game exited early' }
    elseif (-not $complete -and $nStops -gt 0) { $note = 'tour did not complete' }

    $colour = 'Green'
    if ($note) { $colour = 'Red' }
    if ($blanks -gt 0 -and -not $note) { $note = "$blanks blank captures" }
    Write-Host ("      {0}s  {1} stops visited, {2} shots{3}{4}" -f `
                $secs, $visited, $shots,
                $(if ($pushed) { ", $pushed pushed by the world" } else { '' }),
                $(if ($note) { "  <- $note" } else { '' })) -ForegroundColor $colour

    $rows += [pscustomobject]@{
        world = $short; secs = $secs; stops = $nStops; visited = $visited
        shots = $shots; blanks = $blanks; pushed = $pushed; complete = $complete; note = $note
    }
    $rows | Export-Csv -NoTypeInformation -Path $csv
  } catch {
    Write-Host ("      ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    if ($p -and -not $p.HasExited) { try { $p.Kill() } catch {}; $p.WaitForExit(5000) | Out-Null }
  }
}

if ($fake -and -not $fake.HasExited) { try { $fake.Kill() } catch {} }
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'fakehost' } |
    ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force } catch {} }
Remove-Item (Join-Path $Game 'vrtour.txt') -Force -ErrorAction SilentlyContinue

# Put the player's settings back.
if (Test-Path $cfgSnap) { Copy-Item $cfgSnap $cfgLive -Force; Remove-Item $cfgSnap -Force }

Write-Host ''
Write-Host ("summary: {0}" -f $csv) -ForegroundColor Cyan
$rows | Where-Object { $_.note } | Format-Table -AutoSize
