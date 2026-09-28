# Loads EVERY world in the game, one process each, and writes down what the
# renderer made of it.
#
# This is the level-side equivalent of the polygon account: a per-level number
# that nobody has to interpret. Four rounds of testing went into world geometry
# on the strength of two levels; 103 levels is the denominator.
#
# It needs no headset and no person. `+runworld <world>` is an engine console
# variable that has been in NOLF since retail - CGameClientShell::OnEngineInit
# reads it and calls LoadWorld directly - so the menus, the mission list and the
# save system are all skipped. That is the whole trick.
#
#   .\tools\sweep-levels.ps1                    every world in tools\worlds.txt
#   .\tools\sweep-levels.ps1 -Only M01S02       one world, by substring
#   .\tools\sweep-levels.ps1 -Start 40 -Count 10
#   .\tools\sweep-levels.ps1 -Retail            the stock d3d.ren, as a control
#
# Per level it waits for the renderer to print its ACCOUNT and then kills the
# game immediately, so a level costs about as long as it takes to load rather
# than a fixed timeout. A level that never prints one is the interesting case
# and is recorded as such.
#
# Output: logs\sweep\summary.csv plus logs\sweep\<WORLD>\{renstub,client}.log

param(
    [string]$List    = 'tools\worlds.txt',
    [string]$Only    = '',
    [int]   $Start   = 0,
    [int]   $Count   = 0,
    [int]   $TimeoutSec = 90,
    # WHICH LINE MEANS "THIS LEVEL HAS SHOWN ME WHAT I CAME FOR". The
    # account is printed within a couple of seconds, before the client has
    # published a single sprite - so a sweep looking for sprite sizes killed
    # every level before there were any. Anything later can be waited for by
    # name instead of by adding a second timeout.
    [string]$Until   = 'R3D ACCOUNT',
    [int]   $SettleSec  = 3,
    [string]$OutDir  = 'logs\sweep',
    [switch]$Retail,
    # EXTRA SWITCHES, so an A/B can be swept rather than argued about. A rule
    # that deletes geometry has to be answerable over the whole game and not
    # over the two levels somebody happened to load.
    #   -Set @('StubSkipInvisible=1') -OutDir logs\sweep-skipinv
    [string[]]$Set = @(),
    # MEASURE EVERY LEVEL AS THE **SECOND** WORLD OF ITS PROCESS.
    #
    # +runworld gives a level a process to itself, so every sweep this
    # project has ever run measured 103 FIRST worlds. That is a blind spot
    # with a shape: the renderer caches on the engine's world POINTER, the
    # allocator hands the same address back for the next level, and a
    # whole class of fault therefore cannot appear until a second load.
    # One of them left every level after the first unlit and came through
    # a clean 103-world sweep without a mark.
    #
    # With -Anchor the process loads the anchor world first and then the
    # level under test, and the wait counts TWO accounts instead of one.
    # Everything downstream already reads the LAST account, which is the
    # level under test.
    #
    #   -Anchor 'Worlds\T01S01' -OutDir logs\sweep-second
    [string]$Anchor = '',
    [int]   $World2At = 8,
    # A level cannot anchor itself - that would measure a RELOAD of the
    # same world, which is a different question. Levels equal to -Anchor
    # use this instead.
    [string]$AltAnchor = 'Worlds\T01S02',
    [switch]$KeepGoing
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
# THE PLAYER'S SETTINGS ARE NOT THE DESK'S. The engine writes autoexec.cfg
# when the game closes, and this harness closes it gracefully - so a run
# that passed +soundenable 0 left the next headset session with the effects
# volume off and the quality low (9 September). The file is snapshotted
# before the run and put back after; a snapshot left by a killed run is
# restored first, so it can never leak into a headset session (play-vr
# restores it too).
$cfgLive = Join-Path $Game 'autoexec.cfg'
$cfgSnap = Join-Path $Root 'logs\autoexec.headset.cfg'
if (Test-Path $cfgSnap) { Copy-Item $cfgSnap $cfgLive -Force }
elseif (Test-Path $cfgLive) { Copy-Item $cfgLive $cfgSnap -Force }

$LogDir = Join-Path $Game 'logs'
$Out  = Join-Path $Root $OutDir
New-Item -ItemType Directory -Force $Out | Out-Null

$worlds = Get-Content (Join-Path $Root $List) | Where-Object { $_.Trim() }
if ($Only) { $worlds = $worlds | Where-Object { $_ -match [regex]::Escape($Only) } }
if ($Start -gt 0) { $worlds = $worlds | Select-Object -Skip $Start }
if ($Count -gt 0) { $worlds = $worlds | Select-Object -First $Count }
Write-Host ("sweeping {0} worlds, {1} s timeout each" -f $worlds.Count, $TimeoutSec) -ForegroundColor Cyan

# One fake host for the whole sweep. It only publishes a head pose into the
# shared block; nothing in it is per-process, so restarting it 103 times buys
# nothing and costs two seconds each time.
$fakeLog = Join-Path $Root 'logs\fakehost-sweep.log'
New-Item -ItemType Directory -Force (Split-Path $fakeLog) | Out-Null
$fake = Start-Process -FilePath 'python' -ArgumentList @(('"' + (Join-Path $PSScriptRoot 'fakehost.py') + '"')) `
                      -PassThru -WindowStyle Minimized -RedirectStandardOutput $fakeLog
Start-Sleep -Seconds 2
if ($fake.HasExited) { throw "fake host exited immediately (exit $($fake.ExitCode))" }

Add-Type -AssemblyName System.Windows.Forms
$primary = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Primary } | Select-Object -First 1

$rez = @('NOLF.rez','NOLF2.rez','NOLFdll.rez','NOLFl.rez','custom',
         'Nolfu003.rez','Nolfcres003.rez','NolfGoty.rez','Modernizer.rez')

$rows = @()
$csv  = Join-Path $Out 'summary.csv'
$i = 0
$p = $null
# A sweep that stops on the first surprise is not a sweep. One level failing in
# a way the harness did not anticipate must cost that level and nothing else,
# so the per-level body runs non-fatally and records what went wrong.
$ErrorActionPreference = 'Continue'
foreach ($w in $worlds) {
    $i++
  try {
    # Any lithtech.exe but the previous level's own is one this script did
    # not start - someone launched the game mid-run. Leave it alone.
    if (Get-Process lithtech -ErrorAction SilentlyContinue | Where-Object { -not $p -or $_.Id -ne $p.Id }) {
        Write-Host '  STOPPED: another lithtech.exe is running (a player may be in it). It was left alone; the sweep ends here.' -ForegroundColor Red
        break
    }
    # WORLDS/M01S01.DAT -> Worlds\M01S01, which is the form runworld wants.
    $short = ($w -replace '\.DAT$','') -replace '/','\'
    $name  = ($w -replace '.*[/\\]','') -replace '\.DAT$',''
    $tag   = ($w -replace '\.DAT$','') -replace '[/\\]','_'
    Write-Host ("[{0}/{1}] {2}" -f $i, $worlds.Count, $short) -ForegroundColor Yellow

    # A fresh live log, so polling it cannot see the previous level's account.
    $live = Join-Path $LogDir 'renstub.log'
    if (Test-Path $live) { Remove-Item $live -Force -ErrorAction SilentlyContinue }

    $gameArgs = @('-windowtitle','NOLF VR')
    foreach ($r in $rez) { $gameArgs += @('-rez',$r) }
    $gameArgs += @('+multiplayer','0')
    if ($primary) { $gameArgs += @('+CardDesc',$primary.DeviceName) }
    if ($Retail) { $gameArgs += @('+RenderDll','d3d.ren') }
    else {
        # THE SAME FOUR SWITCHES play-vr -Native CARRIES, AND THE SAME SCREEN
        # SIZE. THIS IS THE THIRD SCRIPT AND IT WAS THE ONE STILL OUT OF SYNC:
        # look-shot.ps1 was corrected on 5 September and this was not, so every
        # level in a 103-world sweep ran three switches out of four and took its
        # resolution from whatever the previous run happened to leave in
        # autoexec.cfg. The engine PERSISTS +VR* cvars, so this looks fine right
        # up until someone runs something else first - and a sweep is precisely
        # the thing you run overnight, after something else.
        #
        # Keep all three lists equal: the -Native block in play-vr.ps1, the
        # d3dstub.ren block in look-shot.ps1, and this one.
        $gameArgs += @('+RenderDll','d3dstub.ren','+StubNativeFrustum','1',
                       '+VRAsymFrustum','0','+VRCrosshair','0',
                       '+VRStereo','2')
        $gameArgs += @('+ScreenWidth','2560','+ScreenHeight','1384')
        # SILENT, for the same reason as look-shot.ps1: 103 launches that
        # each start and stop DirectSound wear the audio service down.
        $gameArgs += @('+soundenable','0','+musicenable','0')
    }
    # QUOTED. Three of the 103 have a space in the name - "LithTech Tech Demo",
    # "Treetop Assault_AM", "Guns & Hockeypucks_DM" - and PowerShell's
    # -ArgumentList joins an array with spaces WITHOUT quoting, so those three
    # reached the engine as three arguments and it shut down on an unknown
    # world before the harness could tell the difference from a crash.
    if ($Anchor) {
        # The anchor loads first; the level under test loads on top of it
        # a few seconds later, through the client's VRWorld2 one-shot.
        $useAnchor = $Anchor
        if ($short -ieq $Anchor) { $useAnchor = $AltAnchor }
        $gameArgs += @('+runworld', ('"' + $useAnchor + '"'))
        $gameArgs += @('+VRWorld2', ('"' + $short + '"'),
                       '+VRWorld2At', "$World2At")
    } else {
        $gameArgs += @('+runworld', ('"' + $short + '"'))
    }
    # SPLIT ON COMMAS AND SEMICOLONS, because the caller is often a shell.
    # look-shot.ps1 was fixed for this on 6 September and this script was not,
    # so -Set "ScreenWidth=3840;ScreenHeight=2076" arrived as ONE pair and
    # reached the engine as +ScreenWidth "3840;ScreenHeight=2076". The engine
    # parsed 3840 out of it and ignored the rest, so the width changed, the
    # height did not, and the run looked like it had worked.
    $flat = @()
    foreach ($e in $Set) { $flat += ($e -split '[;,]') | Where-Object { $_.Trim() } }
    foreach ($e in $flat) { $n,$v = $e -split '=',2; $gameArgs += @("+$n", $v) }

    $t0 = Get-Date
    # THE GAME IS DPI-AWARE, WHATEVER WINDOWS DECIDED TONIGHT. At 05:10 on 9 September
    # every capture came back 1.5x its size: the game's window was being scaled by
    # the 150% display, so a 2560x1384 render filled a 3840x2076 window. The layer
    # in the registry says HIGHDPIAWARE and was being ignored; this environment
    # variable is the same layer, applied to this launch only.
    $env:__COMPAT_LAYER = 'HIGHDPIAWARE'
    # ...and the Modernizer's SDL2 makes its own declaration; this is its hint.
    $env:SDL_WINDOWS_DPI_AWARENESS = 'system'
    $p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $gameArgs `
                       -WorkingDirectory $Game -PassThru

    $sawAccount = $false
    $exitedEarly = $false
    for ($s = 0; $s -lt $TimeoutSec; $s++) {
        Start-Sleep -Seconds 1
        if ($p.HasExited) { $exitedEarly = $true; break }
        if (Test-Path $live) {
            # FileShare.ReadWrite, and it is the whole of this poll working.
            # [IO.File]::ReadAllText opens with FileShare.Read, which DENIES
            # other writers - and the renderer has this file open for writing
            # the entire run, so every read threw and every level was recorded
            # as "no account within 90 s" while its log sat there complete.
            try {
                $fs = New-Object System.IO.FileStream($live,
                        [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                        [System.IO.FileShare]::ReadWrite)
                $sr = New-Object System.IO.StreamReader($fs)
                $txt = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
                if ($Anchor) {
                    # NOT A COUNT OF ACCOUNTS. A level is sometimes built
                    # TWICE - the renderer rebuilds when textures were
                    # still arriving - so 'wait for the second account'
                    # can be satisfied entirely by the ANCHOR, and then
                    # the anchor's numbers get filed under the name of the
                    # level under test. Silent, and it looks like data.
                    #
                    # 'R3D WORLD LOADED (2)' is printed once, by the
                    # second world load and nothing else, so waiting for
                    # an account AFTER it cannot be satisfied early.
                    $mark = $txt.IndexOf('R3D WORLD LOADED (2)')
                    if ($mark -ge 0 -and
                        [regex]::IsMatch($txt.Substring($mark), $Until)) {
                        $sawAccount = $true; break
                    }
                } elseif ($txt -match $Until) {
                    $sawAccount = $true; break
                }
            } catch {}
        }
    }
    $secs = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    # Let the frame in flight finish writing before the log is copied.
    if ($sawAccount) { Start-Sleep -Seconds $SettleSec }

    $exitCode = ''
    if ($p.HasExited) { $exitCode = $p.ExitCode }
    else {
        try { $p.CloseMainWindow() | Out-Null } catch {}
        Start-Sleep -Seconds 2
    }
    # Only the process this script started.
    if (-not $p.HasExited) { try { $p.Kill() } catch {}; $p.WaitForExit(5000) | Out-Null }
    Start-Sleep -Milliseconds 800

    $dst = Join-Path $Out $tag
    New-Item -ItemType Directory -Force $dst | Out-Null
    # The renderer writes logs\renstub.log live; the CLIENT writes into a
    # timestamped subdirectory it makes at exit, and copies the renderer's log
    # in beside it. Take the newest such directory when there is one, because
    # it is the only place client.log ever appears.
    $run = Get-ChildItem $LogDir -Directory -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    foreach ($f in @('renstub.log','client.log','renshim.log')) {
        $src = $null
        if ($run -and (Test-Path (Join-Path $run.FullName $f))) { $src = Join-Path $run.FullName $f }
        elseif (Test-Path (Join-Path $LogDir $f)) { $src = Join-Path $LogDir $f }
        if ($src) { Copy-Item $src (Join-Path $dst $f) -Force -ErrorAction SilentlyContinue }
    }

    # ---- what the log says -------------------------------------------------
    $r = [ordered]@{
        world = $short; name = $name; secs = $secs
        loaded = $sawAccount; exitedEarly = $exitedEarly; exitCode = $exitCode
        total=''; drawn=''; collision=''; markerTex=''; markerFlags=''
        noTex=''; noTexTrans=''; occluder=''; malformed=''; unaccounted=''
        worldFile=''; missingTex=''
        objects=''; published=''; dropped=''; meshInst=''; meshTris=''
        bridgeHeld=''; bridgeFailed=''
        pieces=''; whitePieces=''; skinTried=''; skinNamed=''; skinLoaded=''
        skinRecycled=''; note=''
    }
    $rl = Join-Path $dst 'renstub.log'
    if (Test-Path $rl) {
        $t = Get-Content $rl -Raw
        # THE LAST ACCOUNT, NEVER THE FIRST, AND EVERY COLUMN BELOW DEPENDED ON
        # IT. A level is built TWICE: once as the world arrives, before the
        # engine has bound a single texture, and again once it has. The first
        # build therefore drops most of the level for "no texture" - M04S01
        # reads 3317 dropped and 68.2% of its polygons missing a texture object
        # - and the second reads 5. -match returns the FIRST match, so every
        # world-side number this sweep has ever reported was the pre-texture
        # build, and any conclusion drawn from drawn/noTex/UNACCOUNTED was drawn
        # from a state that exists for a fraction of a second and is not what
        # anybody plays. The model-side columns already took the last match and
        # said why; the world side did not.
        function Script:LastNum([string]$text, [string]$pattern) {
            $mm = [regex]::Matches($text, $pattern)
            if ($mm.Count) { return $mm[$mm.Count-1].Groups[1].Value }
            return ''
        }
        # THE LAST ACCOUNT AS A BLOCK, not a loose search over the whole log.
        # Taking the last match was the fix for reading the pre-texture build,
        # and it was right in direction and wrong in detail: there is a SECOND
        # account, R3D MODEL ACCOUNT, with a 'drawn' line of its own. The last
        # match was therefore the MODEL count - "drawn 2" on a level that draws
        # 6485 - and the summary said every level renders 0.3% of its polygons.
        # A number that absurd is a gift; a plausible wrong one gets believed.
        $blk = $t
        $am = [regex]::Matches($t, 'R3D ACCOUNT: the level has')
        if ($am.Count) {
            $bi = $am[$am.Count-1].Index
            $tail = $t.Substring($bi)
            $bm = [regex]::Match($tail.Substring(20), '[\r\n]\s*R3D (?!ACCOUNT)')
            if ($bm.Success) { $blk = $tail.Substring(0, 20 + $bm.Index) }
            else { $blk = $tail }
        }
        $r.total       = LastNum $blk 'R3D ACCOUNT: the level has (\d+) polygons'
        $r.drawn       = LastNum $blk 'drawn\s+(\d+)'
        $r.collision   = LastNum $blk 'collision hull \+ AI volumes\s+(\d+)'
        $r.markerTex   = LastNum $blk 'editor marker textures\s+(\d+)'
        $r.markerFlags = LastNum $blk 'marker surface FLAGS\s+(\d+)'
        $r.noTex       = LastNum $blk 'no texture, dropped\s+(\d+)'
        $r.noTexTrans  = LastNum $blk 'translucent, no texture\s+(\d+)'
        $r.occluder    = LastNum $blk 'occluder texture\s+(\d+)'
        $r.malformed   = LastNum $blk 'malformed in the heap\s+(\d+)'
        $r.unaccounted = LastNum $blk 'UNACCOUNTED\s+(\d+)'
        $r.worldFile   = LastNum $t 'WORLD FILE: (\S+)'
        $r.missingTex  = LastNum $t '(\d+) distinct textures are referenced but missing'
        if ($t -match 'could not be identified or parsed') {
            if ($t -match 'WORLD FILE: \S+ \(\d+ loaded, (\d+) could not') {
                if ($Matches[1] -ne '0') { $r.note = "worldfile unparsed x$($Matches[1])" }
            }
        }
        # The model side. LAST occurrence, not the first: the mesh report is
        # periodic and the first one lands before the player exists.
        $mm = [regex]::Matches($t, 'R3D MESH: (\d+) triangles from \d+ pieces of (\d+) instances')
        if ($mm.Count) {
            $r.meshTris  = $mm[$mm.Count-1].Groups[1].Value
            $r.meshInst  = $mm[$mm.Count-1].Groups[2].Value
        }
        # THE SKIN AUDIT. Last occurrence for the same reason as the mesh
        # report: it is periodic and the first one lands before the level is
        # populated. whitePieces is the column this sweep exists to read -
        # anything but 0 is a level drawing models with no texture.
        $sm = [regex]::Matches($t, 'R3D SKINAUDIT: (\d+) of (\d+) drawn pieces have NO texture[^|]*\| skin slots: (\d+) tried, (\d+) named, (\d+) loaded from file, (\d+) rebuilt')
        if ($sm.Count) {
            $last = $sm[$sm.Count-1]
            $r.whitePieces  = $last.Groups[1].Value
            $r.pieces       = $last.Groups[2].Value
            $r.skinTried    = $last.Groups[3].Value
            $r.skinNamed    = $last.Groups[4].Value
            $r.skinLoaded   = $last.Groups[5].Value
            $r.skinRecycled = $last.Groups[6].Value
        }
        $bm = [regex]::Matches($t, 'bridge held on (\d+) models and failed on (\d+)')
        if ($bm.Count) {
            $r.bridgeHeld   = $bm[$bm.Count-1].Groups[1].Value
            $r.bridgeFailed = $bm[$bm.Count-1].Groups[2].Value
        }
    }
    # The client's own denominator: how many objects the engine has near the
    # camera against how many of them we actually hand the renderer.
    $cl = Join-Path $dst 'client.log'
    if (Test-Path $cl) {
        $c = Get-Content $cl -Raw
        $vm = [regex]::Matches($c, 'VRModels: (\d+) instances, \d+ nodes, (\d+) dropped this frame \(of (\d+) objects')
        if ($vm.Count) {
            $last = $vm[$vm.Count-1]
            $r.published = $last.Groups[1].Value
            $r.dropped   = $last.Groups[2].Value
            $r.objects   = $last.Groups[3].Value
        }
    }
    if (-not $sawAccount) {
        if ($exitedEarly) { $r.note = "GAME EXITED after $secs s (exit $exitCode)" }
        else { $r.note = "NO ACCOUNT within $TimeoutSec s" }
    }
    $rows += [pscustomobject]$r
    $rows | Export-Csv $csv -NoTypeInformation -Encoding UTF8

    $verdict = if ($sawAccount -and $r.unaccounted -eq '0') { 'ok' }
               elseif ($sawAccount) { "UNACCOUNTED $($r.unaccounted)" }
               else { $r.note }
    Write-Host ("      {0}s  {1}  drawn {2}/{3}" -f $secs, $verdict, $r.drawn, $r.total) `
               -ForegroundColor $(if ($verdict -eq 'ok') { 'Green' } else { 'Red' })
    if (-not $sawAccount -and -not $KeepGoing -and $i -eq 1) {
        Write-Host 'first world failed to load - stopping. -KeepGoing runs anyway.' -ForegroundColor Red
        break
    }
  } catch {
    Write-Host ("      HARNESS ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Magenta
    $rows += [pscustomobject]@{ world=$short; name=$name; secs=''; loaded=$false
                                note=("harness error: " + $_.Exception.Message) }
    $rows | Export-Csv $csv -NoTypeInformation -Encoding UTF8
    if ($p -and -not $p.HasExited) { try { $p.Kill() } catch {}; $p.WaitForExit(5000) | Out-Null }
  }
}

if ($fake -and -not $fake.HasExited) { try { $fake.Kill() } catch {} }
Write-Host ""
Write-Host "summary: $csv" -ForegroundColor Cyan
$rows | Where-Object { -not $_.loaded -or $_.unaccounted -ne '0' } |
    Format-Table world, secs, drawn, total, unaccounted, note -AutoSize
# Put the player's settings back (see the note at $cfgSnap).
if (Test-Path $cfgSnap) { Copy-Item $cfgSnap $cfgLive -Force; Remove-Item $cfgSnap -Force }
