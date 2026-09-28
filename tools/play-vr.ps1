# Launches NOLF in VR.
#
#   1. Connect Virtual Desktop to the headset
#   2. Run this
#   3. Put the headset on
#
# The host starts first, waits for the game's window, then submits both eyes to
# the headset. Head tracking drives the camera; the mouse still aims.

# -LagMs is how far behind your head the displayed frame is, end to end.
# Raise it if head tilt shears the image; lower it if the world over-corrects
# and swims.
# -FovYScale stretches (>1) or squashes (<1) the world vertically. Tune it
# until doors are door-shaped; the value that works tells us what the renderer
# is actually doing.
# -ProjMode 0 declares the frustum the game rendered and submits the whole
# image. -ProjMode 1 declares the headset's own asymmetric frustum and submits a
# matching slice of it.
# THE DESKTOP SHOWS THE RIGHT EYE BY DEFAULT (the host's mirror window, over
# the game window). While the headset is live the renderer stops presenting to
# its own window - Present() blocked the game for up to 46 ms at the monitor's
# rate - so without the mirror the desktop kept the last thing it presented,
# the side-by-side splash. -NoMirror turns it off; -Mirror is still accepted.
# -HeadAsMouse starts with the head-as-mouse experiment already ON, instead of
# relying on F1 being pressed in the headset. The crosshair is GREEN with a
# filled centre while it is on, white with a hole while it is off, so which arm
# is running can be seen rather than remembered. See docs/HEAD-AS-MOUSE.md.
# -NoEyeCentre turns OFF the per-eye optical-centre correction (VRAsymFrustum 0),
# which is the only thing in the pipeline that touches the VERTICAL axis and not
# the horizontal. It is a control, not a mode: if pitch and roll still bend with
# it off, the fault is the vertical field, not the centre.
#
# A named switch rather than a -Set passthrough on purpose. Launched with
# `powershell -File`, `-Set a=1,b=2` arrives as ONE string and only the first
# variable is applied - which has already produced one silently wrong A/B here.
# A switch cannot be mis-parsed.
# -PoseLagMs deliberately tells the runtime each image was rendered from a pose
# this many milliseconds old, so it warps the picture forward by that much head
# motion. It is a LEVER on the one quantity left in the pipeline, not a setting:
# 0 asks the runtime to correct almost nothing, 120 asks it to correct five
# times more than the real 25 ms staleness.
#
# The point is to make the reprojection's contribution unmistakable in both
# directions. F7 turned out to be a weak test - it swaps a 25 ms correction for
# a 10-20 ms one, which is why it looked identical to F8.
#
# Sets the host argument AND both client variables together. Set separately they
# fight: the client republishes its own value every frame and silently wins.
# -ProjExact is the geometrically exact construction, and it is ONE switch on
# purpose. It needs three things to agree and they have never all been set at
# once, which is why ProjMode 1 was judged and rejected on 13 August:
#
#   ProjMode 1        the host declares the runtime's OWN asymmetric frustum and
#                     hands over the sub-rectangle of our image that corresponds
#                     to it, instead of declaring our wide symmetric field and
#                     letting the compositor squeeze it down
#   VRAsymFrustum 0   render about the forward axis. The declared frustum is
#                     defined about that axis, so rotating the camera onto the
#                     optical centre as well would double-count it
#   VRFovUniform 1    ask the renderer for exactly the field we publish. The
#                     host's own comment says the sub-rectangle is "only correct
#                     if hx/hy exactly match what was rendered", and at the
#                     shipped VRFovXTest 0.8 the client renders 139 degrees while
#                     publishing 119 - so the slice was computed against an image
#                     1.6x wider than assumed
#
# Set any two of those without the third and the result is worse than doing
# none of them. That is what 14 degrees of eye divergence looked like.
param([string]$Minutes = '60', [int]$LagMs = 0, [double]$FovYScale = 1.0, [int]$ProjMode = 0,
      [switch]$Native,
      # -Retail: run the RETAIL d3d.ren on purpose, as a control. Without it a
      # run that would have used the retail renderer now STOPS. See the guard
      # below the renderer selection.
      [switch]$Retail,
      [switch]$SameEye, [switch]$Mirror, [switch]$NoMirror, [switch]$NoAA, [switch]$HeadAsMouse,
      [switch]$NoEyeCentre, [int]$PoseLagMs = -1, [double]$FovMargin = 0,
      [int]$AsymMode = -1, [switch]$ProjExact,
      [switch]$SharedFrame, [switch]$WindowCapture, [switch]$MenuMono,
      [switch]$NoMusic, [switch]$NoSound,   # the two-second hitch: is it the audio stack?
      [double]$MenuWidth = 0, [double]$MenuDist = 0, [double]$MenuAspect = 1.5, [string[]]$Set = @(),
      # LOAD STRAIGHT INTO A LEVEL, skipping the menus and the save system.
      # +runworld has been in NOLF since retail; this just plumbs it through.
      #   -World "Worlds\M06S01"
      [string]$World = '',
      # And stand somewhere specific in it. See docs and +VRTele.
      #   -At "-994,-100,854"
      [string]$At = '',
      # SECONDS AFTER THE LEVEL IS UP before -At moves you. A level whose opening
      # scene ends by placing the player (the GOTY bonus mission's conv1 sends
      # the player to playerTP1) undoes an early move; wait past it.
      [double]$AtDelay = 4,
      # THE LEVEL TOUR: god mode, every weapon, every mission on the menu -
      # applied on every world entry. Without it the cvar is passed as 0, so a
      # tour never leaks into an ordinary session (the engine saves cvars).
      [switch]$God,
      # PIN THE PLAIN MAIN MENU. The Modernizer picks a different one by the
      # DATE - CInterfaceMgr::GetMainFolder returns the CASUAL layout on a
      # Friday and the WINTER one from 15 December. Same items, different art
      # and different positions. It surfaced mid-session when a Thursday
      # headset run crossed midnight into Friday and the menu came back
      # looking totally different, with items missing or a different color.
      #   -PlainMenu
      [switch]$PlainMenu,
      # TESTING: do not start the OpenXR host. Something else provides the
      # shared block - tools\fakehost.py - so the game's VR paths run at a desk
      # with no headset connected. Everything else is the normal launch.
      [switch]$NoHost,
      # Let the host wait up to this many ms for the game's next frame before
      # repeating the last one (0 = off, the default). See vrmain.cpp.
      [double]$FreshWaitMs = 0)

$ErrorActionPreference = 'Stop'

# ---- -NATIVE IS NOT OPTIONAL, AND WRITING THAT DOWN TWICE DID NOT WORK ----
#
# BEFORE ANYTHING IS LAUNCHED. This check used to sit down beside the renderer
# selection, which is 200 lines after the VR host is started - so refusing the
# run there left an orphaned host process behind. Any guard that can turn a run
# back has to do it before the first Start-Process.
#
# The script has always defaulted to the RETAIL d3d.ren and printed one cyan
# line about it. That line scrolls past while you are connecting Virtual
# Desktop, and the next thing that happens is a headset going on - so the
# warning arrived at the one moment nobody can read it.
#
# What you get instead is stock NOLF at 800x600 with a scrambled 2D layer:
# d3d.ren's known viewport bug, which from the headset reads as a game screen
# completely zoomed in with barely anything visible (19 September) and, before
# that, as a splash screen showing only a zoomed-in part of the logo. Both
# times it cost a headset trip, and both times the trap was ALREADY written
# down in the development notes. A trap
# that is documented and still sprung twice is not a documentation problem, so
# this stops rather than warns.
#
# -Retail still runs it deliberately, which is worth keeping: the retail
# renderer stays available as a control for reading the 2D layer.
#
# Computed here rather than below because the guard needs it here. The renderer
# block further down reads this same variable and must not recompute it.
$wantsStub = $Native -or ($Set | Where-Object { $_ -match '^\s*RenderDll\s*=\s*d3dstub\.ren\s*$' })
if (-not $wantsStub -and -not $Retail) {
    Write-Host ''
    Write-Host '  STOPPED - this would have launched the RETAIL renderer, not ours.' -ForegroundColor Red
    Write-Host ''
    Write-Host '  play-vr.ps1 defaults to d3d.ren. Without -Native you get stock NOLF' -ForegroundColor Yellow
    Write-Host '  at 800x600 with a scrambled 2D layer - the "zoomed in, can barely' -ForegroundColor Yellow
    Write-Host '  see anything" symptom. That is d3d.ren, not a bug in the mod.' -ForegroundColor Yellow
    Write-Host ''
    Write-Host '  What you almost certainly want:' -ForegroundColor Green
    Write-Host '    .\tools\play-vr.ps1 -Native -Mirror' -ForegroundColor Green
    Write-Host ''
    Write-Host '  To run the retail renderer on purpose, as a control:' -ForegroundColor Cyan
    Write-Host '    .\tools\play-vr.ps1 -Retail' -ForegroundColor Cyan
    Write-Host ''
    exit 1
}

# CAUGHT HERE BECAUSE IT KEEPS HAPPENING.
#
#   powershell -File play-vr.ps1 -Native -Set @('A=1','B=2')
#
# -File does NOT evaluate the array. PowerShell hands each element over as a
# separate POSITIONAL argument, so the second one lands on the parameter after
# -Set - which is $Minutes - and the run dies on "Cannot convert value B=2 to
# type System.Int32". That trap is written down in this project's notes and was
# walked into again today, costing a launch.
#
# $Minutes is a STRING purely so this can be caught: an [int] fails at parameter
# BINDING, before a line of this file runs, and there is nowhere to put a useful
# message. Forward slashes in the example on purpose - see the note further down
# about backslashes being eaten by scripted edits.
if ($Minutes -match '=') {
    Write-Host ''
    Write-Host 'WRONG INVOCATION, not a bad switch.' -ForegroundColor Red
    Write-Host ("  '{0}' arrived where the run length goes." -f $Minutes) -ForegroundColor Red
    Write-Host '  -File does not evaluate an array. Use -Command:' -ForegroundColor Yellow
    Write-Host ''
    Write-Host ("  powershell -ExecutionPolicy Bypass -Command ""& '{0}' -Native -Set @('A=1','B=2')""" -f $PSCommandPath) -ForegroundColor Green
    Write-Host ''
    throw 'play-vr: -Set was passed through -File. See above.'
}
[int]$MinutesInt = 0
if (-not [int]::TryParse($Minutes, [ref]$MinutesInt)) {
    throw ("play-vr: -Minutes '{0}' is not a number." -f $Minutes)
}

$Root = Split-Path $PSScriptRoot -Parent
# A desk run that was killed leaves its snapshot of the player's settings in
# logs\autoexec.headset.cfg (see look-shot.ps1). A headset session must start
# from the player's file, never the desk's.
$cfgSnapPV = Join-Path $Root 'logs\autoexec.headset.cfg'
if (Test-Path $cfgSnapPV) {
    Copy-Item $cfgSnapPV (Join-Path $Root 'game\autoexec.cfg') -Force
    Remove-Item $cfgSnapPV -Force
    Write-Host 'restored the player settings a desk run had left behind' -ForegroundColor Yellow
}

# COMPRESSED MUSIC CRASHES THE GAME ON EVERY FOCUS CHANGE. With
# "musictype" "ima" in autoexec.cfg - which a retail install can carry from its
# own launcher's options - the engine's sound library (mss32.dll) faults with
# an access violation the moment the window loses focus: an alt-tab, a
# notification, the headset software taking the foreground. Reproduced at the
# desk on the first alt-tab, three times out of three, and never without the
# key. The engine's default music plays with the key absent, so it goes.
$cfgPV = Join-Path $Root 'game\autoexec.cfg'
if (Test-Path -LiteralPath $cfgPV) {
    try {
        $cfgText = [System.IO.File]::ReadAllText($cfgPV)
        if ($cfgText -match '(?im)^\s*"musictype"\s+"[^"]*"\s*$') {
            $cfgText = [regex]::Replace($cfgText, '(?im)^\s*"musictype"\s+"[^"]*"\s*\r?\n?', '')
            [System.IO.File]::WriteAllText($cfgPV, $cfgText, (New-Object System.Text.ASCIIEncoding))
            Write-Host 'removed "musictype" from game\autoexec.cfg (compressed music crashes the game on focus changes)' -ForegroundColor Yellow
        }
    } catch {
        Write-Host ("could not check game\autoexec.cfg for musictype: {0}" -f $_.Exception.Message) -ForegroundColor Yellow
    }
}

$Exe  = Join-Path $Root 'host\nolfvr.exe'

# NOT SET UP YET. Play before Setup started the host, which then waited a
# minute on a game that could not launch. Say so before anything starts.
if (-not (Test-Path -LiteralPath (Join-Path $Root 'game\lithtech.exe'))) {
    Write-Host ''
    Write-Host 'NOLF is not set up in this folder yet.' -ForegroundColor Red
    Write-Host 'Double-click Setup.bat first - it copies your game in beside the VR files.'
    Write-Host ''
    exit 1
}
if (-not (Test-Path $Exe)) {
    Write-Host 'host\nolfvr.exe is missing - the download is incomplete. Extract the whole zip again.' -ForegroundColor Red
    Write-Host '(Building from source: tools\build-vrhost.ps1.)'
    exit 1
}

# Is the CLIENT stale against the shared block's layout?
#
# The host and the client agree on VRShared.h. When that header changes the
# host is rebuilt as a matter of course - it is one script - but the client is
# a four-minute MSBuild through Modernizer.rez and is easy to forget. When the
# two disagree the client REJECTS the block outright: no head tracking, and the
# renderer draws per-eye frustums while the client does not offset the eyes,
# which reads as double vision.
#
# Not hypothetical. On 3 September the header went to version 13 at 00:51 and
# the rez was last built at 00:32, and a headset session was spent finding it.
# The client said so in its own log on the first frame of every run, including
# the desk runs used to clear the build beforehand. Nobody read the line.
#
# Forward slashes on purpose: this file has had backslashes eaten by three
# different scripted-edit layers now.
$hdr = Join-Path $Root 'src/nolf1-modernizer/NOLF/ClientShellDLL/VRShared.h'
$rez = Join-Path $Root 'game/Modernizer.rez'
if ((Test-Path $hdr) -and (Test-Path $rez)) {
    $hT = (Get-Item $hdr).LastWriteTime
    $rT = (Get-Item $rez).LastWriteTime
    if ($hT -gt $rT) {
        Write-Host ''
        Write-Host 'STALE CLIENT: VRShared.h is NEWER than Modernizer.rez.' -ForegroundColor Red
        Write-Host ("  header {0}   rez {1}" -f $hT, $rT) -ForegroundColor Red
        Write-Host '  The client will reject the shared block: no head tracking,' -ForegroundColor Red
        Write-Host '  and the eyes will disagree. Run tools/build.ps1 first.' -ForegroundColor Red
        Write-Host ''
    } else {
        Write-Host ("client: rez newer than VRShared.h ({0}) - in step" -f $rT) -ForegroundColor DarkGray
    }
}


# Before the host launches, because its lag is argv[2] and the process starts
# further down. The client half of the same switch is applied with the other
# console variables at the bottom.
if ($PoseLagMs -ge 0) { $LagMs = $PoseLagMs }
if ($ProjExact)      { $ProjMode = 1 }

$same = if ($SameEye) { 1 } else { 0 }
$mir  = if ($NoMirror) { 0 } else { 1 }
Write-Host "Starting VR host (lag $LagMs ms, proj $ProjMode, sameEye $same, mirror $mir)..." -ForegroundColor Cyan
$aa = if ($NoAA) { 0 } else { 1 }

# argv[8] is the wire version (0 = ours) and argv[9] the frame source, so the
# placeholder has to be passed to reach the one after it.
# THE SHARED TEXTURE IS THE DEFAULT FROM 8 SEPTEMBER. -WindowCapture is the
# way back.
#
# The game has been rendering at 90 fps all along - its own frame log says
# ft=11ms on every line of the last headset run - and the host was only
# receiving 55 to 59 fresh frames a second, repeating a third of them. The host
# said so in that run, once every ten seconds:
#
#   capture: 552 fresh, 335 repeated (55.2 fresh/sec, 38% repeated)
#      <- REPEATS ARE THE STUTTER
#
# That is a third of frames being shown twice while the head keeps moving,
# which is exactly why motion looked uneven. It is the WINDOW CAPTURE
# path, not the game: the renderer hands its finished frame over directly when
# this is on, and no frame can be missed or repeated.
#
# Never fatal. If the shared texture does not come up the host falls back to
# window capture on its own, which is what it did before today.
$shared = if ($WindowCapture) { 0 } else { 1 }
if ($WindowCapture) {
    Write-Host 'FRAME SOURCE: window capture (the old path - expect ~58 fps of' -ForegroundColor Yellow
    Write-Host '  fresh frames into a 90 Hz headset, a third of them repeats).' -ForegroundColor Yellow
}
if (-not $WindowCapture) {
    Write-Host 'FRAME SOURCE: the renderer''s shared texture, not window capture.' -ForegroundColor Green
    Write-Host '  Every frame the game draws, once - against ~60 a second with a' -ForegroundColor Green
    Write-Host '  third repeated. Desk-measured at 90.1 fps; NOT yet seen in a' -ForegroundColor Green
    Write-Host '  headset. If anything looks wrong, drop -SharedFrame to compare.' -ForegroundColor Yellow
    Write-Host '  The host says which source it actually used - read that line.' -ForegroundColor Yellow
}
# -MenuMono: the pre-3-September menu - the whole side-by-side window sent to
# both eyes. The default now gives each eye its own half, because the renderer
# draws the 2D layer into both halves. That is the one change in
# docs/PER-EYE-2D.md that could NOT be tested at the desk, so this is the way
# back and it is one flag: if menus look doubled, add -MenuMono.
$menuStereo = if ($MenuMono) { 0 } else { 1 }

# -MenuWidth / -MenuDist size the floating menu panel, in metres. The host
# defaults to 2.6 m wide at 1.6 m away - about 78 degrees across, against the
# 53 of the 2-at-2 it replaced, which was too small to read the options.
# Named arguments, appended after the positional ten, so neither
# list can shift the other.
$hostArgs = @($MinutesInt, $LagMs, $FovYScale, $ProjMode, $same, $mir, $aa, 0, $shared, $menuStereo)
if ($MenuWidth -gt 0) { $hostArgs += @('--menu-width', $MenuWidth) }
if ($MenuDist  -gt 0) { $hostArgs += @('--menu-dist',  $MenuDist)  }
# -MenuAspect 0 presents the whole eye as the panel, the pre-13 September shape.
$hostArgs += @('--menu-aspect', $MenuAspect)
# THE SPECTATOR VIEW on the desktop: steadied and level, or the plain eye.
# Options > VR > Desktop view saves VRSpectator in autoexec.cfg; the host reads
# it at launch, so a change applies next start. -Set VRSpectator=0 wins.
$spec = 1
$specCfg = Join-Path $Root 'game\autoexec.cfg'
if (Test-Path -LiteralPath $specCfg) {
    $specLine = Select-String -LiteralPath $specCfg -Pattern '^"VRSpectator"\s+"([\d.]+)"' | Select-Object -First 1
    if ($specLine) { $spec = [int][double]$specLine.Matches[0].Groups[1].Value }
}
$specSet = $Set | Where-Object { $_ -match '^\s*VRSpectator\s*=\s*([\d.]+)' } | Select-Object -First 1
if ($specSet -and $specSet -match '([\d.]+)\s*$') { $spec = [int][double]$Matches[1] }
$hostArgs += @('--spectator', $(if ($spec -ne 0) { 1 } else { 0 }))
if ($NoHost) {
    Write-Host 'NO HOST: the OpenXR host is not started (-NoHost); something else must provide the shared block.' -ForegroundColor Yellow
} else {
    # The host reads this from its environment, which it inherits from here.
    $env:NOLFVR_FRESH_WAIT_MS = [string]$FreshWaitMs
    if ($FreshWaitMs -gt 0) { Write-Host ("FRESH-FRAME WAIT: up to {0} ms" -f $FreshWaitMs) -ForegroundColor Green }
    # The host writes the headset's recommended eye size here (see below); a
    # file left by an earlier session must not be mistaken for this one's.
    $eyeFile = Join-Path $Root 'headset-eye.txt'
    Remove-Item -LiteralPath $eyeFile -Force -ErrorAction SilentlyContinue
    # A RUNNING STEAMVR IS THE HEADSET THE PLAYER MEANT. A game started from
    # Steam gets XR_RUNTIME_JSON from SteamVR; one started from this folder
    # gets the system's runtime, and on a PC that also streams a Quest that is
    # Virtual Desktop - which waited for a Quest while a Steam Frame sat on his
    # head with SteamVR up. So with SteamVR running (vrserver) and no runtime
    # already chosen, the host is pointed at SteamVR for THIS launch only; the
    # system setting is not touched, and with SteamVR not running nothing changes.
    if (-not $env:XR_RUNTIME_JSON) {
        $vrs = Get-Process vrserver -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($vrs) {
            $svRoot = $null
            # ...\SteamVR\bin\win64\vrserver.exe -> ...\SteamVR
            try { if ($vrs.Path) { $svRoot = Split-Path (Split-Path (Split-Path $vrs.Path -Parent) -Parent) -Parent } } catch { }
            if (-not $svRoot) {
                try {
                    $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
                    $svRoot = Join-Path ($steam -replace '/', '\') 'steamapps\common\SteamVR'
                } catch { }
            }
            $svManifest = if ($svRoot) { Join-Path $svRoot 'steamxr_win64.json' } else { $null }
            if ($svManifest -and (Test-Path -LiteralPath $svManifest)) {
                $env:XR_RUNTIME_JSON = $svManifest
                Write-Host ("SteamVR is running: VR goes through SteamVR for this launch ({0})" -f $svManifest) -ForegroundColor Green
            } else {
                Write-Host 'SteamVR is running but its OpenXR manifest was not found; using the system''s OpenXR runtime.' -ForegroundColor Yellow
            }
        }
    }
    Start-Process -FilePath $Exe -ArgumentList $hostArgs -WorkingDirectory $Root
    Start-Sleep -Seconds 4
    for ($i = 0; $i -lt 8 -and -not (Test-Path -LiteralPath $eyeFile); $i++) { Start-Sleep -Milliseconds 500 }
}

# THE WORLD RENDERS AT THE HEADSET'S RESOLUTION. The game's screen stays at
# run.ps1's 3840x2076 - its menus, HUD and subtitles are laid out for that and
# its bitmap font code cannot go past about 5,000 px a strip, so the engine
# itself cannot run at a headset's size - and the renderer is told to give it
# more PIXELS than coordinates (+StubRenderScale100): the world fills them, the
# 2D is laid out exactly as before. The scale comes from the eye height the
# headset asked the host for, times Options > VR > Resolution % (the
# VRResolution line the engine saves in autoexec.cfg; 100 when absent). It was
# 1920x2076 an eye against the 3072x3264 a Quest 3 asks for: a tester found the
# picture soft and the plants pixelated. With no headset yet the host cannot
# say, and the scale stays 100. An explicit -Set StubRenderScale100=... wins.
$eyeFile = Join-Path $Root 'headset-eye.txt'
if (-not ($Set | Where-Object { $_ -match '^\s*StubRenderScale100\s*=' }) -and (Test-Path -LiteralPath $eyeFile)) {
    $rec = (Get-Content -LiteralPath $eyeFile -TotalCount 1) -split '\s+'
    $recW = 0; $recH = 0
    if ($rec.Count -ge 2 -and [int]::TryParse($rec[0], [ref]$recW) -and [int]::TryParse($rec[1], [ref]$recH) -and $recH -gt 0) {
        $pct = 100
        $cfgFile = Join-Path $Root 'game\autoexec.cfg'
        if (Test-Path -LiteralPath $cfgFile) {
            $line = Select-String -LiteralPath $cfgFile -Pattern '^"VRResolution"\s+"([\d.]+)"' | Select-Object -First 1
            if ($line) { $pct = [int][double]$line.Matches[0].Groups[1].Value }
        }
        if ($pct -lt 60) { $pct = 60 }
        if ($pct -gt 125) { $pct = 125 }
        # The game's screen height is run.ps1's 2076 unless the caller set one.
        $modeH = 2076
        $sh = $Set | Where-Object { $_ -match '^\s*ScreenHeight\s*=\s*(\d+)' } | Select-Object -First 1
        if ($sh -and $sh -match '(\d+)\s*$') { $modeH = [int]$Matches[1] }
        $scale = [int][math]::Round(100.0 * $recH * $pct / 100.0 / $modeH)
        if ($scale -lt 100) { $scale = 100 }
        if ($scale -gt 200) { $scale = 200 }
        $Set = @("StubRenderScale100=$scale") + $Set
        Write-Host ("resolution: world at {0}% ({1}x{2} per eye) - {3}% of the headset's {4}x{5}" -f `
            $scale, [int](1920 * $scale / 100), [int]($modeH * $scale / 100), $pct, $recW, $recH) -ForegroundColor Cyan
    }
}

# RENDERER SWITCHES THAT OPTIONS > VR SAVES. The renderer reads its switches
# from its command line only, so a row that writes one into autoexec.cfg acts
# only once the launcher passes it on - "Show body (restart)" saved StubBody
# and nothing ever read it back. An explicit -Set wins.
$swCfg = Join-Path $Root 'game\autoexec.cfg'
# Show body is saved as VRShowBody: 1.0 saved the old row as StubBody 0 in
# every install, and that would keep the body off after an upgrade.
foreach ($pair in @(@('VRShowBody', 'StubBody'), @('StubMirrorBody', 'StubMirrorBody'))) {
    $cfgName = $pair[0]; $k = $pair[1]
    if ($Set | Where-Object { $_ -match ('^\s*' + $k + '\s*=') }) { continue }
    if (-not (Test-Path -LiteralPath $swCfg)) { continue }
    $swLine = Select-String -LiteralPath $swCfg -Pattern ('^"' + $cfgName + '"\s+"([\d.]+)"') | Select-Object -First 1
    if ($swLine) { $Set += ('{0}={1}' -f $k, [int][double]$swLine.Matches[0].Groups[1].Value) }
}

# Give the game window the keyboard, and keep trying for a few seconds.
#
# In VR the game is a borderless window on a desktop nobody can see, and if this
# PowerShell window keeps focus then every key goes to it instead. The symptom
# is total: no movement, no fire, no menu - the game simulating happily with
# nothing reaching it. Reported from the headset on 28 August as "no controls
# and no movement" while the log showed up=1 and the world rendering normally.
#
# You cannot click on a window you cannot see, so this does it.
Start-Job -ScriptBlock {
    $sh = New-Object -ComObject WScript.Shell
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        $g = Get-Process lithtech -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($g -and $g.MainWindowHandle -ne 0) {
            try { $sh.AppActivate($g.Id) | Out-Null } catch {}
        }
    }
} | Out-Null

$vars = @()
if ($HeadAsMouse) {
    Write-Host "HEAD-AS-MOUSE arm: the crosshair will be GREEN with a filled centre." -ForegroundColor Green
    $vars += 'VRHeadAsMouse=1'
}
if ($NoEyeCentre) {
    Write-Host "EYE-CENTRE CONTROL: VRAsymFrustum 0 - the per-eye optical centre is OFF." -ForegroundColor Green
    Write-Host "  The image may sit differently in the lenses. Judge ONLY whether it bends." -ForegroundColor Green
    $vars += 'VRAsymFrustum=0'
}
if ($PoseLagMs -ge 0) {
    Write-Host "POSE-LAG LEVER: the runtime is told each image is $PoseLagMs ms old." -ForegroundColor Green
    Write-Host "  At 120 the world will lag and swim badly. That is the lever working," -ForegroundColor Green
    Write-Host "  not the defect. Judge ONLY whether straight lines BEND." -ForegroundColor Green
    $vars += 'VRExactPose=0'
    $vars += "VRPoseLag=$PoseLagMs"
}
if ($FovMargin -gt 0) {
    # How much WIDER than the headset's own frustum we render and declare.
    # Default 1.10 submits a 119x121 degree flat plane for a headset whose eye
    # frustum is 94x99 - a very wide plane for a compositor to resample into a
    # much narrower one, and the resample is head-FIXED, so world features sweep
    # through whatever error it has as the head turns. That is invisible on the
    # monitor and only exists in the headset, which is the shape of the report.
    #
    # Below about 0.90 the periphery goes black during head motion, because
    # there is no image beyond what the headset shows. That is the cost of the
    # lever, not the defect.
    Write-Host "FOV MARGIN: $FovMargin (default 1.10)." -ForegroundColor Green
    Write-Host "  Narrow values darken the edges when you move. Judge ONLY whether" -ForegroundColor Green
    Write-Host "  straight lines BEND in the middle of the view." -ForegroundColor Green
    $vars += "VRFovMargin=$FovMargin"
}
if ($AsymMode -ge 0) {
    # 1 = render each eye about its own VERTICAL optical centre only (shipped).
    # 2 = also about the HORIZONTAL one, which is what lets the required field
    #     drop from max(54,40)=54 degrees to the half-span (54+40)/2=47.
    #
    # Mode 2 was tried before the client published the rotation it actually
    # applied, so the two ends disagreed and it produced a double image (the
    # eyes diverging). The client now publishes fAppliedYawRad and the
    # host declares it verbatim, which is exactly the mismatch that caused it -
    # but it is the one risk in this change, so it gets judged on its own.
    Write-Host "EYE-CENTRE MODE: VRAsymFrustum $AsymMode" -ForegroundColor Green
    if ($AsymMode -ge 2) {
        Write-Host "  Mode 2+ adds the HORIZONTAL centre. If you get DOUBLE VISION," -ForegroundColor Yellow
        Write-Host "  stop and say so - that is a known past failure of this mode." -ForegroundColor Yellow
    }
    $vars += "VRAsymFrustum=$AsymMode"
}
if ($ProjExact) {
    Write-Host 'PROJ EXACT: declaring the headset''s own frustum and submitting the' -ForegroundColor Green
    Write-Host '  matching slice. Render about the forward axis, asked field = published field.' -ForegroundColor Green
    Write-Host '  The view will look NARROWER. That is the correct geometry, not a fault.' -ForegroundColor Green
    $vars += 'VRAsymFrustum=0'
    $vars += 'VRFovUniform=1'
}

# -Native: our own renderer, building each eye's true asymmetric frustum.
#
# All four settings together or none of them. VRAsymFrustum must be 0 or the
# client rotates the camera onto the eye's optical centre while the renderer
# ALSO offsets the frustum, and the two corrections add. VRCrosshair must be
# 0 because the per-eye crosshair is painted at the centre of each half -
# which was the forward direction under a symmetric frustum and is about 15
# degrees off it under this one.
# -Set: anything extra, passed straight through to the game's console, so a
# headset session can adjust without a rebuild. Added LAST so it overrides the
# switches above rather than being overridden by them.
#
#   -Set StubSkipBlackTint=1     the world is black but the HUD is there
#   -Set StubHudScale100=80      the HUD is too big or too near the edge
#   -Set StubStereo2D=0          the A of the per-eye 2D A/B
#   -Set StubSkipHidden=0        put the collision hull and AI volumes back
$vars += ('VRCheats=' + $(if ($God) { '7' } else { '0' }))
$vars += ('NoFunMenus=' + $(if ($PlainMenu) { '1' } else { '0' }))
# THE DEBUG SWITCHES START FROM A KNOWN STATE. Options > VR persists them into
# autoexec.cfg like any option, and a desk test that set one leaked it into the
# next headset session (all seven were found on, from the morning's runs). So
# every launch says what they are: all off, or with -God the testing set - god,
# arsenal and all-missions lit by the client from VRCheats at world entry, and
# the level skip and the position readout lit here.
foreach ($k in @('VRDebugGod','VRDebugArsenal','VRDebugMissions','VRDebugSkip','VRDebugPos','VRDebugClip','VRDebugNoAI')) {
    $on = $God -and ($k -eq 'VRDebugSkip' -or $k -eq 'VRDebugPos')
    $vars += ($k + '=' + $(if ($on) { '1' } else { '0' }))
}
if ($God) { Write-Host 'LEVEL TOUR: god mode, everything, all missions unlocked' -ForegroundColor Green }
$extraVars = $Set

$renderer = 'd3d.ren'

# Our renderer IMPLIES the pairing. -Native is not the only way to select it:
# the engine persists RenderDll into autoexec.cfg, and -Set RenderDll=d3dstub.ren
# selects it directly, and neither of those carries the three switches it needs.
# A session ran exactly that way, drew mono, and was then compared against a
# -Native run and called a regression. The switches travel with the renderer now.
#
# $wantsStub is computed at the TOP of this file, beside the -Native guard,
# because that guard has to run before the VR host is started. Do not recompute
# it here - two copies of this expression is how they come to disagree.
if ($wantsStub -and -not $Native) {
    Write-Host 'NATIVE FRUSTUM: d3dstub.ren was asked for without -Native;' -ForegroundColor Yellow
    Write-Host '  adding the three switches that must accompany it.' -ForegroundColor Yellow
}
if ($wantsStub) {
    $renderer = 'd3dstub.ren'
    # Anything the caller set explicitly wins; these only fill in the gaps.
    # VRStereo is PINNED, not left to the file. The engine persists cvars in
    # autoexec.cfg, so tools\compare-ours.ps1 setting it to 0 for a flat
    # comparison would otherwise make the NEXT headset run silently mono.
    # SOUND ON, PINNED. The engine persists soundenable and musicenable into
    # autoexec.cfg, and the desk harness runs SILENT on purpose (hundreds of
    # launches starting and stopping DirectSound degraded the audio service
    # into a 100 ms hitch every two seconds - cleared only by a reboot). A
    # headset run must never inherit the desk's silence from the file.
    # -NoMusic / -NoSound: one run each, to tell the audio stack's hitch from
    # everything else. The stall watchdog in the renderer names the module
    # regardless; these are the control arms.
    $soundPin = 'soundenable=1'; $musicPin = 'musicenable=1'
    if ($NoSound) { $soundPin = 'soundenable=0'; $musicPin = 'musicenable=0'; Write-Host 'SOUND OFF for this run (-NoSound)' -ForegroundColor Yellow }
    elseif ($NoMusic) { $musicPin = 'musicenable=0'; Write-Host 'MUSIC OFF for this run (-NoMusic)' -ForegroundColor Yellow }
    foreach ($pair in @('StubNativeFrustum=1', 'VRAsymFrustum=0',
                        'VRCrosshair=0', 'VRStereo=2',
                        $soundPin, $musicPin)) {
        $name = ($pair -split '=')[0]
        if (-not ($Set | Where-Object { $_ -match "^\s*$name\s*=" })) { $vars += $pair }
        else { Write-Host "  $name left to your -Set" -ForegroundColor Yellow }
    }
}
if ($Native) {
    Write-Host 'NATIVE FRUSTUM: our renderer, each eye with its own asymmetric' -ForegroundColor Green
    Write-Host '  projection. The crosshair is OFF on purpose - it is drawn at the' -ForegroundColor Green
    Write-Host '  centre of each half, which is no longer the forward direction.' -ForegroundColor Green
    Write-Host '  Confirmed in the headset on 4 September: bending, double vision,' -ForegroundColor Green
    Write-Host '  the black box and the flickering are all closed. The models are' -ForegroundColor Yellow
    Write-Host '  what is left - NPCs and the menu backdrop draw flat or not at all.' -ForegroundColor Yellow
}
elseif (-not $wantsStub) {
    # Only reachable with -Retail now: the guard above turns back anything else.
    Write-Host 'RENDERER: the retail d3d.ren, ON PURPOSE (-Retail). Expect an' -ForegroundColor Cyan
    Write-Host '  800x600 window and a scrambled 2D layer - that is its viewport bug,' -ForegroundColor Cyan
    Write-Host '  and it is why this arm is only useful for reading the 2D layer.' -ForegroundColor Cyan
}

# The log is the record of which arm ran, not this console window - a run has
# already been judged on the assumption a switch took when it had not.
if ($World) {
    $vars += ("runworld=" + $World)
    Write-Host ("LEVEL: loading straight into {0}" -f $World) -ForegroundColor Green
}
if ($At) {
    $tx, $ty, $tz = $At -split ','
    $vars += @('VRTele=1', "VRTeleX=$tx", "VRTeleY=$ty", "VRTeleZ=$tz", "VRTeleAt=$AtDelay")
    Write-Host ("STAND AT: {0}" -f $At) -ForegroundColor Green
}

$vars += $extraVars
foreach ($v in $extraVars) { Write-Host "EXTRA: $v" -ForegroundColor Magenta }

if ($vars.Count) {
    & (Join-Path $PSScriptRoot 'run.ps1') -Set $vars -RenderDll $renderer
} else {
    & (Join-Path $PSScriptRoot 'run.ps1') -RenderDll $renderer
}

$run = Get-ChildItem (Join-Path $Root 'logs') -Directory -Filter 'vrhost-*' -EA SilentlyContinue |
       Sort-Object Name | Select-Object -Last 1
if ($run) {
    Write-Host "`n=== host log: $($run.Name) ===" -ForegroundColor Cyan
    # The host may still be running and holding the file open, so read with
    # sharing rather than letting Get-Content fail on a locked file.
    $path = Join-Path $run.FullName 'vrhost.log'
    try {
        $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object System.IO.StreamReader($fs)
        $lines = $sr.ReadToEnd() -split "`r?`n"
        $sr.Close(); $fs.Close()
        $lines | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
    } catch {
        Write-Host "  (could not read log: $($_.Exception.Message))" -ForegroundColor Yellow
    }
}
