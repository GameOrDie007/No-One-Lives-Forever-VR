# ---------------------------------------------------------------------------
# chain-levels.ps1 - play every mission's scenes in ONE process, leaving each
# level by ITS OWN EXIT, unattended.
#
# WHY THIS EXISTS. Every harness this project had loaded one level per process
# (the sweep, the tour) or a second by name (sweep -Anchor). None of them ever
# went through the door the player goes through: the level's exit trigger,
# MID_PLAYER_EXITLEVEL to the server, ExitLevel() on the client, the loading
# screen and ITS THREAD. The first real headset session crashed on exactly that
# door - the HQ elevator - on the third world of the process, and no
# automated run had ever opened it.
#
# The client does the leaving (VRWorld2Exit 1, VRWorld2At N seconds after each
# world entry, VRWorld2Count up to 9 times); this script starts one process per
# mission at its first scene, keeps the window in front so the engine does not
# pause, and watches the renderer log for world loads and crashes. A mission's
# last scene exits to the mission summary folder, which is where the chain
# ends: no new load for -IdleSec means "done", and the next mission starts in
# a fresh process.
#
#   .\tools\chain-levels.ps1                       every mission
#   .\tools\chain-levels.ps1 -Missions T01,M01     the Assignment, then Morocco
#   .\tools\chain-levels.ps1 -EverySec 20 -Set @('soundenable=1')
# ---------------------------------------------------------------------------
param(
    [string[]]$Missions = @(),
    [int]   $EverySec = 14,
    [int]   $IdleSec  = 75,   # a scene may open with a 30 s cinematic before the one-shot can fire
    [int]   $MaxSec   = 480,
    [string]$OutDir   = 'logs\chain',
    [string[]]$Set    = @(),
    [int]   $ResW = 1280, [int] $ResH = 692,
    # STEAL THE FOCUS ON PURPOSE, every N seconds. 0 is off.
    #
    # The engine frees and reloads the render DLL on every focus loss and
    # gain, and that is also where DirectSound loses and restores its
    # buffers. The three mss32 crashes this project has ever recorded all
    # happened in one arm that lost focus 45 times to my own progress
    # polling; two clean arms of ten missions each, 90 transitions apiece,
    # produced none. So focus loss is the condition worth reproducing
    # deliberately rather than the nuisance worth avoiding. A console
    # window opening and closing is what did it originally, so that is
    # what this does.
    [int]   $StealFocusEvery = 0
)

$ErrorActionPreference = 'Continue'
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'
$Out  = Join-Path $Root $OutDir
New-Item -ItemType Directory -Force $Out | Out-Null

# ---- which levels start a mission ---------------------------------------
$names = Get-Content (Join-Path $PSScriptRoot 'level-names.txt') |
         Where-Object { $_ -and -not $_.StartsWith('#') }
$firsts = @()
foreach ($l in $names) {
    $w, $label = $l -split "`t", 2
    if ($label -match 'scene 1 of' -or $label -notmatch 'scene') {
        $short = ($w -split '\\')[-1].ToUpper()
        $firsts += [pscustomobject]@{ world = $w; short = $short;
                                      label = ($label -replace '\s*\(scene.*\)', '').Trim() }
    }
}
# -File hands "T01,M01" over as ONE string, so split whatever arrived.
$Missions = @($Missions | ForEach-Object { $_ -split '[,; ]' } | Where-Object { $_ })
if ($Missions.Count) {
    # @(...): one match is otherwise a bare object with no .Count, and 'no missions matched'.
    $firsts = @($firsts | Where-Object { $m = $_.short; ($Missions | Where-Object { $m -like "$_*" }).Count -gt 0 })
}
if (-not $firsts.Count) { throw 'no missions matched' }

# ---- the fake host, so the client believes a headset is live ------------
$fakeLog = Join-Path $Root 'logs\fakehost-chain.log'
Get-Process lithtech -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'fakehost' } |
    ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force } catch {} }
$fake = Start-Process -FilePath 'python' `
        -ArgumentList @((Join-Path $PSScriptRoot 'fakehost.py'), '--rhand', '0,0,0') `
        -PassThru -WindowStyle Minimized -RedirectStandardOutput $fakeLog
Start-Sleep -Seconds 2
if ($fake.HasExited) { throw "fake host exited immediately (exit $($fake.ExitCode))" }

# KEEP THE GAME IN FRONT. The engine pauses on focus loss and game time stops
# with it; a run at the desk with the window behind is a run that does nothing.
Add-Type -Name Fg -Namespace ChainW -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int n);
[DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
'@
function Bring-Front($proc) {
    # THE ENGINE TEARS THE RENDERER DOWN ON EVERY FOCUS LOSS and rebuilds it
    # on focus gain (LTEVENT 6/4 then 7/5/3 - measured, focus-exp.ps1). So
    # focus is not about pausing, which the engine does not do; it is about
    # not restarting the renderer under the test. Restore focus with the one
    # call that costs nothing - SetForegroundWindow - and never ShowWindow
    # (a resize message) or an ALT tap (a menu-mode message), both of which
    # the engine also answers with a restart.
    try { $proc.Refresh() } catch { return $false }
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { return $false }
    if ([ChainW.Fg]::GetForegroundWindow() -eq $proc.MainWindowHandle) { return $true }
    if ([ChainW.Fg]::IsHungAppWindow($proc.MainWindowHandle)) { return $false }
    [ChainW.Fg]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 300
    return ([ChainW.Fg]::GetForegroundWindow() -eq $proc.MainWindowHandle)
}
function Log-IsOurs($path, $proc) {
    # IS THIS LOG THE ONE THIS PROCESS IS WRITING?
    #
    # It is not always. The renderer moves an old log aside on a new launch and
    # opens a fresh one - unless the previous process is still holding the file,
    # which happens exactly when the previous mission CRASHED. The move fails,
    # the open fails, the new process logs nowhere, and the stale file is still
    # sitting there looking like a log. The harness then counts the PREVIOUS
    # run's world loads and crash marker as this mission's.
    #
    # That is not hypothetical: in the first control arm of 11 September,
    # mission 2 was credited with a log whose own command line said it had been
    # launched with +runworld worlds\m13s01, from a run hours earlier, and
    # mission 3 opened at "world 6 loaded at 2 s" because six of mission 2's
    # loads were still in the file.
    #
    # A PID ALONE IS NOT ENOUGH - Windows reuses them within minutes, and a
    # recycled pid is exactly how the renderer was fooled into appending to the
    # previous run's log in the first place. The header now carries the
    # process's CREATION TIME as well; both must match.
    $t = Read-Shared $path
    if (-not $t) { return $false }
    $m = [regex]::Match($t, '\(pid (\d+) start (\d+)\)')
    if (-not $m.Success) { return $false }
    if ([int]$m.Groups[1].Value -ne $proc.Id) { return $false }
    try { $want = $proc.StartTime.ToFileTime() } catch { return $false }
    # A second of tolerance: the two sides read the same FILETIME through
    # different APIs and only the low bits could ever disagree.
    return ([math]::Abs([int64]$m.Groups[2].Value - [int64]$want) -lt 10000000)
}
function Read-Shared($path) {
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    try {
        $fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $sr = New-Object System.IO.StreamReader($fs)
        $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $t
    } catch { return '' }
}

$rez = @('NOLF.rez','NOLF2.rez','NOLFdll.rez','NOLFl.rez','custom','Nolfu003.rez',
         'Nolfcres003.rez','NolfGoty.rez','Modernizer.rez')
$env:__COMPAT_LAYER = 'HIGHDPIAWARE'
$rows = @()
$renLive = Join-Path $Game 'logs\renstub.log'

Write-Host ("chaining {0} missions, a new scene every {1} s by the level's own exit" -f $firsts.Count, $EverySec)
$mi = 0
foreach ($m in $firsts) {
    $mi++
    Write-Host ("[{0}/{1}] {2}  ({3})" -f $mi, $firsts.Count, $m.short, $m.label) -ForegroundColor Cyan
    # THE PREVIOUS PROCESS MUST BE GONE BEFORE THE LOG CAN BE MOVED. A crashed
    # one lingers for a second or two holding the file, and both the delete here
    # and the renderer's own rotation then fail silently.
    for ($w = 0; $w -lt 10; $w++) {
        $alive = @(Get-Process lithtech -ErrorAction SilentlyContinue)
        if (-not $alive.Count) { break }
        foreach ($q in $alive) { try { $q.Kill() } catch {} }
        Start-Sleep -Milliseconds 500
    }
    for ($w = 0; $w -lt 6 -and (Test-Path -LiteralPath $renLive); $w++) {
        Remove-Item -LiteralPath $renLive -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $renLive) { Start-Sleep -Milliseconds 500 }
    }
    if (Test-Path -LiteralPath $renLive) {
        Write-Host '      the previous log could not be removed - something still holds it' -ForegroundColor Yellow
    }
    $before = @(Get-ChildItem (Join-Path $Game 'logs') -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })

    $a = @('-windowtitle', 'NOLF VR CHAIN')
    foreach ($r in $rez) { $a += @('-rez', $r) }
    $a += @('+multiplayer','0','+RenderDll','d3dstub.ren','+StubNativeFrustum','1',
            '+VRAsymFrustum','0','+VRCrosshair','0','+VRStereo','2',
            '+ScreenWidth',"$ResW",'+ScreenHeight',"$ResH",'+StubPresentEvery','1')
    if (-not ($Set | Where-Object { $_ -match 'soundenable' })) { $a += @('+soundenable','0','+musicenable','0') }
    $a += @('+runworld', ('"' + $m.world + '"'))
    $a += @('+VRWorld2Exit','1','+VRWorld2At',"$EverySec",'+VRWorld2Count','9','+VRCheats','7')
    foreach ($e in $Set) { foreach ($kv in ($e -split '[;,]')) {
        # A VALUE WITH SPACES MUST ARRIVE AS ONE ARGUMENT. The 3D sound
        # providers are named things like 'DirectSound3D 7+ Software -
        # Full HRTF'; unquoted, the engine takes the first word and
        # silently selects nothing.
        if ($kv.Trim()) {
            $n,$v = $kv -split '=',2
            if ($v -and $v -match ' ') { $v = '"' + $v + '"' }
            $a += @("+$n", $v)
        } } }

    $t0 = Get-Date
    $p = Start-Process -FilePath (Join-Path $Game 'lithtech.exe') -ArgumentList $a -WorkingDirectory $Game -PassThru
    $loads = 0; $lastLoadAt = $t0; $crash = $false; $note = ''; $hungFor = 0; $restarts = 0
    $mine = $false          # has the live log been confirmed to be THIS process's?
    $lastSteal = Get-Date
    # A PERSON AT THE KEYBOARD WINS. Re-fronting the game every two seconds
    # is right for an empty room and hostile to anyone in it: on 21 September
    # the player came back mid-run, tried to pause some music, and the game
    # took the foreground back seven times in one mission. So the
    # loop counts the times the foreground was found elsewhere; after three
    # inside a minute it stops fronting the game for the rest of the run,
    # says so, and the run's numbers carry the renderer restarts that follow.
    $lostAt = @(); $yielded = $false
    while ($true) {
        Start-Sleep -Seconds 2
        $elapsed = ((Get-Date) - $t0).TotalSeconds
        if ($p.HasExited) { $note = "GAME EXITED (exit $($p.ExitCode))"; break }
        if ($StealFocusEvery -gt 0 -and
            ((Get-Date) - $lastSteal).TotalSeconds -ge $StealFocusEvery) {
            $lastSteal = Get-Date
            Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','exit' -WindowStyle Normal | Out-Null
            Start-Sleep -Milliseconds 400
        }
        if (-not $yielded) {
            try { $p.Refresh() } catch {}
            $elsewhere = ($p.MainWindowHandle -ne [IntPtr]::Zero) -and
                         ([ChainW.Fg]::GetForegroundWindow() -ne $p.MainWindowHandle)
            if ($elsewhere -and $StealFocusEvery -le 0) {
                $now = Get-Date
                $lostAt = @($lostAt | Where-Object { ($now - $_).TotalSeconds -lt 60 }) + $now
                if ($lostAt.Count -ge 3) {
                    $yielded = $true
                    Write-Host '      someone is using the PC - not taking the foreground back for the rest of this run' -ForegroundColor Yellow
                }
            }
            if (-not $yielded) { Bring-Front $p | Out-Null }
        }
        # NOTHING IS COUNTED FROM A LOG THAT IS NOT OURS. See Log-IsOurs.
        if (-not $mine) {
            $mine = Log-IsOurs $renLive $p
            if (-not $mine) {
                if ($elapsed -gt 40) {
                    $note = 'LOG NOT OURS (a previous process still holds it) - NOT COUNTED'
                    Write-Host ('      ' + $note) -ForegroundColor Red
                    try { $p.Kill() } catch {}
                    break
                }
                continue
            }
        }
        $txt = Read-Shared $renLive
        $n = ([regex]::Matches($txt, 'R3D WORLD LOADED \(')).Count
        if ($n -gt $loads) { $loads = $n; $lastLoadAt = Get-Date
            Write-Host ("      world {0} loaded at {1:N0} s" -f $n, $elapsed) }
        if ($p.MainWindowHandle -ne [IntPtr]::Zero -and [ChainW.Fg]::IsHungAppWindow($p.MainWindowHandle)) {
            $hungFor += 2
            if ($hungFor -ge 20) { $crash = $true; $note = 'HUNG (main thread not answering for 20 s)'; break }
        } else { $hungFor = 0 }
        $restarts = ([regex]::Matches($txt, 'renderer RE-INITIALISED')).Count
        if ($txt.Contains('=== CRASH ===')) { $crash = $true; $note = 'CRASH (see renstub.log)'; Start-Sleep -Seconds 3; break }
        if ($loads -ge 1 -and ((Get-Date) - $lastLoadAt).TotalSeconds -gt ($IdleSec + $EverySec)) { $note = 'chain ended (mission summary or last scene)'; break }
        if ($elapsed -gt $MaxSec) { $note = "timeout $MaxSec s"; break }
    }
    if (-not $p.HasExited) { try { $p.CloseMainWindow() | Out-Null } catch {}; Start-Sleep -Seconds 3 }
    if (-not $p.HasExited) { try { $p.Kill() } catch {} }
    Start-Sleep -Seconds 1

    $dst = Join-Path $Out $m.short
    New-Item -ItemType Directory -Force $dst | Out-Null
    if (Test-Path -LiteralPath $renLive) { Copy-Item -LiteralPath $renLive -Destination (Join-Path $dst 'renstub.log') -Force }
    $newDirs = Get-ChildItem (Join-Path $Game 'logs') -Directory -ErrorAction SilentlyContinue |
               Where-Object { $before -notcontains $_.Name } | Sort-Object Name | Select-Object -Last 1
    $trans = 0
    if ($newDirs) {
        $cl = Join-Path $newDirs.FullName 'client.log'
        if (Test-Path $cl) { Copy-Item $cl (Join-Path $dst 'client.log') -Force
                             $trans = ([regex]::Matches((Get-Content $cl -Raw), 'VRWorld2: transition')).Count }
    }
    $row = [pscustomobject]@{ mission = $m.short; secs = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1);
                              loads = $loads; exits = $trans; restarts = $restarts; crash = $crash;
                              counted = $mine; note = $note }
    $rows += $row
    Write-Host ("      {0} loads, {1} exits taken, {2} renderer restarts, {3}" -f $loads, $trans, $restarts, $note) -ForegroundColor $(if ($crash) { 'Red' } else { 'Green' })
}

if ($fake -and -not $fake.HasExited) { try { $fake.Kill() } catch {} }
$rows | Export-Csv (Join-Path $Out 'summary.csv') -NoTypeInformation
Write-Host ''
$rows | Format-Table -AutoSize | Out-String | Write-Host
Write-Host ("summary: {0}" -f (Join-Path $Out 'summary.csv'))
exit 0
