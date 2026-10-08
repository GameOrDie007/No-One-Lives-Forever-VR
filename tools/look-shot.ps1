# A desk capture from a chosen HEAD DIRECTION.
#
# The harness has never been able to turn the camera - every judgement about
# what is or is not on screen has been made from wherever the quick save happened
# to be facing, which is usually the floor. fakehost.py has taken
# `--static yaw,pitch,roll` all along and nothing used it.
#
# So: point the head, load the level, photograph it. That is what settles
# "the sky is drawn but nobody can see it" without a headset.
#
#   .\tools\look-shot.ps1 -Look "0,50,0" -Out logs\up.png
#   .\tools\look-shot.ps1 -Look "0,50,0" -World "Worlds\M01S02" -Set @('StubSkyBox=0')

param(
    [string]  $Look   = '0,0,0',      # yaw,pitch,roll in degrees
    [string]  $Out    = 'logs\look.png',
    [string]  $World  = '',           # empty = quick-load the save instead
    [int]     $Wait   = 18,
    # MORE THAN ONE PICTURE, -ShotGapMs apart: <Out>-2.png, -3.png ... for
    # anything judged by whether it MOVES (water, sprites). One still cannot.
    [int]     $Shots  = 1,
    [int]     $ShotGapMs = 250,
    [string[]]$Set    = @(),
    # DELIBERATELY 2560x1384, WHICH IS NO LONGER WHAT THE GAME SHIPS AT.
    #
    # run.ps1 defaults to 3840x2076 since 8 September. This does not, on
    # purpose: every desk capture ever taken was at 2560x1384, and changing the
    # default here would silently make new captures incomparable with all of
    # them. Pass -ResW 3840 -ResH 2076 to photograph what the headset gets.
    #
    # This is the one place the three launch scripts are allowed to disagree.
    [int]     $ResW = 2560,
    [int]     $ResH = 1384,
    [string]  $Renderer = 'd3dstub.ren',  # 'd3d.ren' for the retail control
    [switch]  $Menu,                      # stop at the MAIN MENU: no world, no quick load
    # KEYS SENT AFTER THE WAIT AND BEFORE THE SHOT, one step at a time.
    # This is how an in-game state that only a keypress can reach gets
    # photographed - the PAUSE MENU above all, which no capture harness could
    # reach until now and which is therefore the least tested screen we have.
    #   -Keys '{ESC}'  -KeyDelay 3
    [string[]]$Keys   = @(),
    [int]     $KeyDelay = 3,              # seconds after each key
    # POINT THE RIGHT CONTROLLER, in degrees, the way -Look points the head.
    #
    # Without this the fake host left both hands at orientation (0,0,0), which
    # is not "no hand" - it is a hand pointing exactly where the head points,
    # so a weapon placed by the HAND and one placed by the VIEW came out
    # identical and hand aiming could not be tested at the desk at all.
    #   -RHand "30,0,0"   aims a quarter-turn right of the view
    [string]  $RHand  = '',
    [string]  $RHandPos = '',
    # SWING THE GUN WHILE CAPTURING. "30,0.5" sweeps the right hand's yaw 30
    # degrees at half a hertz - 94 deg/s at the peak, about a person swinging a
    # rifle. A still hand cannot show a pose that is one frame stale, and this
    # harness held its hand still for every capture it has ever taken: the
    # muzzle flash was placed from LAST frame's gun pose and three captures at
    # three fixed angles all looked right, while in the headset it slid off the
    # barrel the moment the gun moved. The error is r * dTheta and dTheta was always
    # zero here.
    [string]  $RHandSweep = '',
    # THE LEFT HAND, and the pair as a HANDLEBAR: -LHandPos "-0.3,1.0,-0.4"
    # -RHandPos "0.3,1.0,-0.2" is the right grip pulled back, a right turn.
    # -Grip squeezes both grips, which is how VRVehicleSteer 1 takes the bars.
    [string]  $LHand  = '',
    [string]  $LHandPos = '',
    [switch]  $Grip,
    # Hold -Stick only from this many seconds in; release it at -StickUntil
    # and hold it again from -StickAgain (walk to a vehicle, X, ride).
    [double]  $StickAt = 0,
    [double]  $StickUntil = 0,
    [double]  $StickAgain = 0,
    [double]  $RStickAt = 0,
    # Fake a recenter (see fakehost --recenter-at) this many seconds in.
    [double]  $RecenterAt = 0,
    # HOLD CONTROLLER BUTTONS. 1 trigger, 2 grip, 4 A/X, 8 B/Y, 16 thumbclick.
    # Pressed a few seconds IN, not from frame 1: the client tracks button
    # EDGES, and a button already held when the client first looks has no edge
    # to find.
    # SQUEEZE A TRIGGER (analog), from -TriggerAt seconds: the vehicle throttle.
    [string]  $Trigger = '',
    [double]  $TriggerAt = 0,
    [int]     $Buttons = 0,
    [double]  $ButtonsAt = 6.0,
    # WALK. fakehost has taken --stick all along and nothing here exposed it,
    # so every desk measurement has been taken standing still - which is
    # exactly the difference between the harness and a person playing, and it
    # is where the shell casings go.
    #   -Stick "0,1"   walks forward
    [string]  $Stick  = '',
    [string]  $RStick = '',                   # right stick x,y held (the weapon wheel)
    [ValidateSet('both','left','right')]
    [string]  $ButtonsHand = 'both',
    # MOUNT THE UPSCALE PACK, from packs\. Opt-in rather than default: it is
    # 2.4 GB of 32-bit textures and it changes what the CUT-OUT rule measures,
    # so a run with it is a different experiment and has to say so.
    # The author's own instruction is that the pack loads AFTER Modernizer,
    # because the last mount wins.
    [switch]  $Pack,
    # EXTRA ARCHIVES WHERE THE PRODUCT MOUNTS game\custom: after the retail
    # ones and before Modernizer.rez (run.ps1). A file outside the game folder
    # mounts fine, e.g. -ExtraRez '..\packs\HD-TEX1-X4.REZ'.
    [string[]]$ExtraRez = @(),
    # WHICH HALF, for bisecting. HD-COMMON1 holds the 2D layer's art -
    # STATBAR, MENU and INTERFACE - as well as guns, characters and
    # attachments; HD-COMMON2 is PROPS only. The crash phase names the 2D
    # layer, so which half it is in is one run rather than an argument.
    #   -Pack -PackSet common2
    [ValidateSet('all','common1','common2')]
    [string]  $PackSet = 'all',
    # STAND SOMEWHERE ELSE. "x,y,z" in world units, applied a few seconds after
    # the level is up, using the engine's own teleport. This is what makes a
    # place that is not a spawn point photographable at the desk - M06S01's
    # double door is at (-994 -128 854) and its spawn is 4000 units away.
    #   -At "-994,-100,854"
    # THE HEADSET'S FIELD, e.g. -EyeFov '59.4,61.3' (see fakehost --eye-fov).
    [string]  $EyeFov = '',
    [string]  $At = '',
    [double]  $AtDelay = 4.0,
    # ANOTHER WINDOW TAKES THE FOCUS this many seconds in, for 4 s: what
    # Virtual Desktop, Steam or a notification does to a player in VR.
    [int]     $StealFocusAt = 0,
    # A MOVING HEAD. fakehost sweeps the orientation by default and this script
    # has always overridden that with --static, which is right for a repeatable
    # capture and useless for anything that has to prove a picture is LIVE
    # rather than frozen. Pause at the same moment in two runs and capture at
    # different ones: a frozen backdrop is identical, a live one is not.
    [switch]  $Sweep,
    # MOUNT A DIRECTORY, for bisecting inside an archive without writing a .rez
    # writer. The stock list already carries `custom`, which has no extension -
    # LithTech mounts folders as well as archives - so a subset extracted by
    # tools\pack-subset.py can be mounted the same way.
    #   -PackDir packs\subset-guns
    [string]  $PackDir = '',
    # END THE LEVEL'S OPENING CAMERA, N seconds in. Default ON for a -World
    # load, because +runworld leaves that camera LIVE for the whole level and
    # the client then sits in the alternative-camera state: player camera in
    # CHASE, first-person weapon hidden AND disabled. Every capture this
    # harness has ever taken of a runworld level was of a state no player is
    # ever in - see docs and the development notes section 5. 0 restores the old behaviour.
    [double]  $EndCinematic = 3.0,
    # THE FAKE HOST'S IPD, in metres. -1 leaves fakehost's own default.
    # Zero puts both eyes at ONE POINT, which - with a symmetric frustum
    # (+StubNativeFrustum 0) - makes the two halves of the frame two renders of
    # the same camera. Every pixel that then differs is a fault in our pass:
    # something sampled, seeded or clocked per PASS instead of per FRAME. That
    # is how the per-eye animation clock was found. See tools\eye-identity.py.
    [double]  $Ipd = -1,
    # PHYSICAL PLAY AT THE DESK: drive both hands from a fakehost key file
    # (see fakehost --script) starting -ScriptAt seconds in, once the world is
    # up. -Pos fixes the fake head's position (x,y,z metres) so slot positions
    # do not depend on the head's yaw.
    [string]  $Script = '',
    [double]  $ScriptAt = 14.0,
    # Restart the script every this many seconds (a soak that keeps playing).
    [double]  $ScriptLoop = 0,
    [string]  $Pos = '',
    # NO HEADSET: start no fake host, so the game runs as it would with no
    # VR runtime at all (it must still start, flat).
    [switch]  $NoHost
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'
# THE PLAYER'S SETTINGS ARE NOT THE DESK'S. The engine writes autoexec.cfg
# when the game closes, and this harness closes it gracefully - so a run
# that passed +soundenable 0 left the next headset session with the effects
# volume off and the quality low (9 September). The file is snapshotted
# before the run and put back after; a snapshot left by a killed run is
# restored first, so it can never leak into a headset session (play-vr
# restores it too).
$cfgLive = Join-Path $Game 'autoexec.cfg'
$cfgSnap = Join-Path $Root 'logs\autoexec.headset.cfg'
# A fresh install has no logs folder yet; the snapshot below needs one.
New-Item -ItemType Directory -Force (Split-Path $cfgSnap) | Out-Null
if (Test-Path $cfgSnap) { Copy-Item $cfgSnap $cfgLive -Force }
elseif (Test-Path $cfgLive) { Copy-Item $cfgLive $cfgSnap -Force }


# GDI+ reports a MISSING DIRECTORY as "A generic error occurred", an
# ExternalException with nothing in it that names a path. One capture was
# lost to that, and the game was left running because the throw skipped the
# kill at the end of this script.
$OutDir = Split-Path (Join-Path $Root $Out) -Parent
if ($OutDir) { New-Item -ItemType Directory -Force $OutDir | Out-Null }

# NEVER KILL A GAME THIS SCRIPT DID NOT START. A running lithtech.exe is most
# likely a player's session; stop and say so instead of closing it.
if (Get-Process lithtech -ErrorAction SilentlyContinue) {
    Write-Host '  STOPPED: the game is already running (a player may be in it). Close it first; nothing was touched.' -ForegroundColor Red
    exit 1
}
# ONLY THIS TREE'S FAKE HOST: another project's desk run may be using its own.
$myFake = Join-Path $PSScriptRoot 'fakehost.py'
Get-Process python   -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine.Contains($myFake) } | ForEach-Object { try { $_.Kill() } catch {} }
Start-Sleep -Milliseconds 600

$fakeLog = Join-Path $Root 'logs\fakehost-look.log'
New-Item -ItemType Directory -Force (Split-Path $fakeLog) | Out-Null
# QUOTED: Start-Process joins -ArgumentList with spaces and quotes nothing
# (PowerShell 5.1), so a tools folder with a space in its path split in two
# and python was handed half a file name.
$fakeArgs = @(('"' + (Join-Path $PSScriptRoot 'fakehost.py') + '"'))
if ($Sweep) { }                      # no --static: fakehost sweeps by itself
else        { $fakeArgs += @('--static', $Look) }
if ($Ipd -ge 0) { $fakeArgs += @('--ipd', "$Ipd") }
if ($EyeFov)   { $fakeArgs += @('--eye-fov', $EyeFov) }
if ($RHand)    { $fakeArgs += @('--rhand', $RHand) }
if ($RHandPos) { $fakeArgs += @('--rhand-pos', $RHandPos) }
if ($RHandSweep) { $fakeArgs += @('--rhand-sweep', $RHandSweep) }
if ($LHand)    { $fakeArgs += @('--lhand', $LHand) }
if ($LHandPos) { $fakeArgs += @('--lhand-pos', $LHandPos) }
if ($Grip)     { $fakeArgs += @('--grip') }
if ($StickAt -gt 0) { $fakeArgs += @('--stick-at', "$StickAt") }
if ($StickUntil -gt 0) { $fakeArgs += @('--stick-until', "$StickUntil") }
if ($StickAgain -gt 0) { $fakeArgs += @('--stick-again', "$StickAgain") }
if ($RStickAt -gt 0) { $fakeArgs += @('--rstick-at', "$RStickAt") }
if ($RecenterAt -gt 0) { $fakeArgs += @('--recenter-at', "$RecenterAt") }
if ($Stick)    { $fakeArgs += @('--stick', $Stick) }
if ($RStick)   { $fakeArgs += @('--rstick', $RStick) }
if ($Buttons)  { $fakeArgs += @('--buttons', "$Buttons", '--buttons-at', "$ButtonsAt", '--buttons-hand', $ButtonsHand) }
if ($Trigger)  { $fakeArgs += @('--trigger', $Trigger, '--trigger-at', "$TriggerAt") }
if ($Script)   { $fakeArgs += @('--script', ('"' + (Resolve-Path $Script).Path + '"'), '--script-at', "$ScriptAt") }
if ($Script -and $ScriptLoop -gt 0) { $fakeArgs += @('--script-loop', "$ScriptLoop") }
if ($Pos)      { $fakeArgs += @('--pos', $Pos) }
$fake = $null
if (-not $NoHost) {
    $fake = Start-Process -FilePath 'python' `
            -ArgumentList $fakeArgs `
            -PassThru -WindowStyle Minimized -RedirectStandardOutput $fakeLog
    Start-Sleep -Seconds 2
    if ($fake.HasExited) { throw "fake host exited (exit $($fake.ExitCode))" }
}

Add-Type -AssemblyName System.Windows.Forms
$primary = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Primary } | Select-Object -First 1
$rez = @('NOLF.rez','NOLF2.rez','NOLFdll.rez','NOLFl.rez','custom',
         'Nolfu003.rez','Nolfcres003.rez','NolfGoty.rez') + $ExtraRez + @('Modernizer.rez')
if ($Pack) {
    # LAST, so it overrides. HD-COMMON only - the HD-TEX halves ship rewritten
    # .DAT levels and this renderer parses those itself.
    if ($PackSet -ne 'common2') { $rez += '..\packs\HD-COMMON1-X4.REZ' }
    if ($PackSet -ne 'common1') { $rez += '..\packs\HD-COMMON2-X4.REZ' }
}
# A DIRECTORY MOUNT ONLY WORKS INSIDE THE GAME FOLDER. Pointing -rez at
# ..\packs\subset-guns mounted NOTHING and said nothing about it: the run
# looked healthy and the weapon came out byte-identical to a run without
# the pack, which is how a mount that silently does nothing gets mistaken
# for a subset that is safe. Name a folder under game\ instead.
if ($PackDir) { $rez += $PackDir }

$a = @('-windowtitle','LOOK')
foreach ($r in $rez) { $a += @('-rez',$r) }
$a += @('+multiplayer','0')
if ($primary) { $a += @('+CardDesc',$primary.DeviceName) }
$a += @('+RenderDll', $Renderer)
if ($Renderer -eq 'd3dstub.ren') {
    # THE SAME FOUR SWITCHES play-vr -Native CARRIES, AND THE SAME SCREEN SIZE.
    #
    # This list was missing +VRStereo 2 and the screen dimensions, so every
    # desk capture ran a configuration the headset never runs. It LOOKED right
    # only because the engine persists cvars into autoexec.cfg: whatever the
    # last -Native run left behind was inherited. That is not a control, it is
    # a coincidence that survives until someone runs something else - and it is
    # how four separate "verified at the desk" claims reached the headset and
    # showed no change. A harness that does not reproduce the arm under test cannot
    # falsify anything. See the -Native block in play-vr.ps1; keep them equal.
    # THE FIRST OCCURRENCE OF A SWITCH WINS, so this cannot be hardcoded if
    # -Set is ever to override it: a run asking for StubNativeFrustum=0 kept
    # the 1 written here, rendered two ASYMMETRIC eyes, and made an
    # eye-agreement test read 90% different and mean nothing.
    $nf = '1'
    foreach ($e in $Set) { if ($e -match 'StubNativeFrustum\s*=\s*0') { $nf = '0' } }
    $a += @('+StubNativeFrustum',$nf,'+VRAsymFrustum','0','+VRCrosshair','0',
            '+VRStereo','2')
    # RESOLUTION, overridable. The per-eye aspect wants to stay at 0.925 for
    # the native per-eye frustum - see tools\set-res.ps1 for that derivation -
    # so pass both or neither.
    $a += @('+ScreenWidth', "$ResW", '+ScreenHeight', "$ResH")
    # SILENT. Nothing at the desk listens, and every launch that starts and
    # stops DirectSound wears the Windows audio service: after a night of
    # sweeps the game hitched 100 ms every two seconds until a reboot. The
    # headset launcher pins sound back ON, so this cannot leak into play.
    # -Set soundenable=1 overrides it for a run that wants audio.
    if (-not ($Set | Where-Object { $_ -match 'soundenable' })) {
        $a += @('+soundenable', '0', '+musicenable', '0')
    }
    # THE DESK STILL PRESENTS. In play the renderer no longer presents to the
    # desktop while a host is live (the headset is the only picture); this
    # harness reads the WINDOW for its screenshots, so it asks for every frame.
    if (-not ($Set | Where-Object { $_ -match 'StubPresentEvery' })) {
        $a += @('+StubPresentEvery', '1')
    }
}
if ($At) {
    $tx, $ty, $tz = $At -split ','
    # A LITTLE ABOVE the point asked for: a teleport to a floor-height y lands
    # inside the floor and the world pushes it somewhere else.
    $a += @('+VRTele','1','+VRTeleX',"$tx",'+VRTeleY',"$ty",'+VRTeleZ',"$tz",
            '+VRTeleAt', "$AtDelay")
}
if ($EndCinematic -gt 0 -and $World) {
    $a += @('+VRDebugEndCinematic', "$EndCinematic")
}
if ($Menu)        { }   # neither: the game settles on the main menu by itself
elseif ($World)   { $a += @('+runworld', ('"' + $World + '"')) }
else              { $a += @('+VRAutoQuickLoad','1') }
# ONE STRING, SPLIT HERE, because the caller is usually a shell that cannot
# build a PowerShell array. `-Set "a=1","b=2"` from bash arrives as the single
# string "a=1,b=2" and the second setting is silently swallowed into the first
# one's VALUE - which is how a controlled A/B ran the same arm twice and
# reported two identical numbers as a result. Accept commas or semicolons.
$setList = @()
foreach ($e in $Set) { $setList += ($e -split '[;,]') }
foreach ($e in $setList) {
    if (-not $e) { continue }
    $n,$v = $e -split '=',2
    if (-not $v) { $v = '1' }
    # A VALUE WITH SPACES HAS TO ARRIVE AS ONE ARGUMENT. chain-levels learned
    # this and look-shot did not, so a run asking for the 3D sound provider
    # 'DirectSound3D 7+ Software - Full HRTF' handed the engine the single word
    # 'DirectSound3D', nothing matched, and the log said the provider was
    # selected because it only recorded the ASKING.
    if ($v -match ' ') { $v = '"' + $v + '"' }
    $a += @("+$n", $v)
}

Write-Host ("  look $Look   " + $(if ($Menu) { 'MAIN MENU' } elseif ($World) { $World } else { 'quick save' })) -ForegroundColor Cyan
# THE GAME IS DPI-AWARE, WHATEVER WINDOWS DECIDED TONIGHT. At 05:10 on 9 September
# every capture came back 1.5x its size: the game's window was being scaled by
# the 150% display, so a 2560x1384 render filled a 3840x2076 window. The layer
# in the registry says HIGHDPIAWARE and was being ignored; this environment
# variable is the same layer, applied to this launch only.
$env:__COMPAT_LAYER = 'HIGHDPIAWARE'
# ...and the Modernizer's SDL2 makes its own declaration; this is its hint.
$env:SDL_WINDOWS_DPI_AWARENESS = 'system'
$p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $a `
                   -WorkingDirectory $Game -PassThru
$null = $p.Handle    # held now, or ExitCode reads empty once the process is gone
for ($s = 0; $s -lt $Wait; $s++) {
    Start-Sleep -Seconds 1
    if ($StealFocusAt -gt 0 -and $s -eq $StealFocusAt -and -not $p.HasExited) {
        # The game has to HAVE the focus to lose it: launched from a visible
        # console, it never had it, and the steal took nothing from it.
        Add-Type -Name FgS -Namespace W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
'@ -ErrorAction SilentlyContinue
        $p.Refresh()
        $front = $false
        for ($try = 0; $try -lt 6 -and -not $front; $try++) {
            [W.FgS]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
            Start-Sleep -Milliseconds 500
            $front = ([W.FgS]::GetForegroundWindow() -eq $p.MainWindowHandle)
        }
        Write-Host "  the game is in front: $front; another window takes the focus for 4 s" -ForegroundColor Cyan
        Start-Process powershell -ArgumentList '-NoProfile', '-Command', 'Start-Sleep 4' | Out-Null
    }
    if ($p.HasExited) {
        # The exit code says how it ended: 0 is a clean quit by the game itself.
        Write-Host "  game exited after $s s, exit code $($p.ExitCode)" -ForegroundColor Red
        break
    }
}
# The keys, once the world is up. Focus first: SendKeys goes to the foreground
# window and the game is not it after a Start-Process.
# THE KEYS GO NOWHERE UNLESS THE GAME IS IN FRONT. SetForegroundWindow is
# refused to a process that is not itself in the foreground, and when it was
# refused the keys went to whatever window WAS in front - two captures in a
# row came back as the untouched main menu, and the DOWN/ENTER had been
# typed into something else. Now: try, check with GetForegroundWindow, retry,
# and if the game still is not in front, REFUSE to send anything.
#
# NO ALT TAP. It used to send one between tries to win the foreground, but
# that key goes to whatever window IS in front - by definition not the game -
# and a person working in it gets their menu bar opened under them.
if (-not $p.HasExited -and $Keys.Count) {
    $p.Refresh()
    $focused = $false
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
        Add-Type -Name Fg2 -Namespace W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
'@
        for ($try = 0; $try -lt 6 -and -not $focused; $try++) {
            [W.Fg2]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
            [W.Fg2]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
            Start-Sleep -Milliseconds 700
            $focused = ([W.Fg2]::GetForegroundWindow() -eq $p.MainWindowHandle)
        }
    }
    if (-not $focused) {
        Write-Host "  REFUSED: the game window could not be brought to the front; keys NOT sent" -ForegroundColor Red
        $Keys = @()
    }
    foreach ($k in $Keys) {
        if ($p.HasExited) { Write-Host "  game exited before '$k'" -ForegroundColor Red; break }
        # EVERY key, not just the first: focus can move mid-sequence (a click,
        # a notification), and a key sent then lands in someone else's window.
        if ([W.Fg2]::GetForegroundWindow() -ne $p.MainWindowHandle) {
            Write-Host "  STOPPED before '$k': the game is no longer in front; the rest NOT sent" -ForegroundColor Red
            break
        }
        Write-Host "  sending $k" -ForegroundColor Cyan
        [System.Windows.Forms.SendKeys]::SendWait($k)
        Start-Sleep -Seconds $KeyDelay
    }
}

if (-not $p.HasExited) {
    # A FAILED GRAB MUST NOT SKIP THE CLEAN-UP BELOW. A minimized window made
    # window-shot throw, and the game and the fake host were left running.
    try {
        & (Join-Path $PSScriptRoot 'window-shot.ps1') -Out (Join-Path $Root $Out) -Width $ResW -Height $ResH |
            ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkGray }
        for ($n = 2; $n -le $Shots -and -not $p.HasExited; $n++) {
            Start-Sleep -Milliseconds $ShotGapMs
            & (Join-Path $PSScriptRoot 'window-shot.ps1') -Out ((Join-Path $Root $Out) -replace '\.png$', "-$n.png") -Width $ResW -Height $ResH |
                ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkGray }
        }
    } catch { Write-Host "    window grab failed: $_" -ForegroundColor Yellow }
}
try { $p.CloseMainWindow() | Out-Null } catch {}
Start-Sleep -Seconds 2
# Only the process this script started.
if (-not $p.HasExited) { try { $p.Kill() } catch {} }
if ($fake -and -not $fake.HasExited) { try { $fake.Kill() } catch {} }

$live = Join-Path $Game 'logs\renstub.log'
if (Test-Path $live) {
    Copy-Item $live ((Join-Path $Root $Out) -replace '\.png$','-renstub.log') -Force -ErrorAction SilentlyContinue
}
# Put the player's settings back (see the note at $cfgSnap).
if (Test-Path $cfgSnap) { Copy-Item $cfgSnap $cfgLive -Force; Remove-Item $cfgSnap -Force }
