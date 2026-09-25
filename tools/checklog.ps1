# Summarises the most recent run log into a pass/fail verdict.
# Usage: checklog.ps1 [-Run <log dir name>] [-Expect <world renders per frame>]
#
# Everything is written with Write-Host so ordering survives piping - mixing
# Write-Host with pipeline output scrambled the report when redirected.

param([string]$Run, [int]$Expect = 0)

$ErrorActionPreference = 'Stop'
$logs = Join-Path (Split-Path $PSScriptRoot -Parent) 'game\logs'

$dir = if ($Run) { Join-Path $logs $Run }
       else { (Get-ChildItem $logs -Directory | Sort-Object Name | Select-Object -Last 1).FullName }
if (-not $dir -or -not (Test-Path $dir)) { throw "No log directory found under $logs" }

$log = Join-Path $dir 'client.log'
Write-Host "Run: $(Split-Path $dir -Leaf)" -ForegroundColor Cyan

$lines = Get-Content $log
foreach ($l in $lines) {
    if ($l -match '^\[' -and $l -notmatch 'Per-frame columns|Deviations are marked') {
        Write-Host ("  " + ($l -replace '^\[\s*[\d.]+\]\s*', ''))
    }
}

# How many world renders per frame should this run have produced? The engine
# logs the mode whenever it changes, so the log states its own expectation
# rather than us assuming one.
$modes = @()
foreach ($l in $lines) {
    if ($l -match 'world renders per frame -> (\d+)') { $modes += [int]$Matches[1] }
}
$distinct = $modes | Sort-Object -Unique

if ($Expect -gt 0) {
    $expect = $Expect
    Write-Host "`nExpected world renders per frame: $expect (from -Expect)" -ForegroundColor Cyan
} elseif ($distinct.Count -eq 1) {
    $expect = $distinct[0]
    Write-Host "`nExpected world renders per frame: $expect (engine-reported)" -ForegroundColor Cyan
} elseif ($distinct.Count -gt 1) {
    Write-Host "`nMIXED RUN - VRDoubleRender was toggled mid-run (modes: $($distinct -join ', '))." -ForegroundColor Yellow
    Write-Host "Frame counts below are still shown, but no verdict is possible." -ForegroundColor Yellow
    Write-Host "Re-run without toggling, or pass -Expect <n> to judge one mode." -ForegroundColor Yellow
    $expect = -1
} else {
    $expect = 1
    Write-Host "`nExpected world renders per frame: 1 (no mode line in log - pre-M2 build)" -ForegroundColor Cyan
}

# The invariants that matter are upper bounds on anything that steps the
# simulation. The world may legitimately be drawn more than once; the frame
# time, the UpdatePlaying call site and the present may not.
function Test-HardViolation($ftr, $up, $wr, $fs, $expect) {
    if ($ftr -gt 1 -or $up -gt 1 -or $fs -gt 1) { return $true }
    if ($expect -lt 0) { return $false }
    if ($up -eq 0) { return ($wr -ne 0) }
    return ($wr -ne $expect)
}

$combos = @{}
$total = 0; $hard = 0; $soft = 0
foreach ($l in $lines) {
    if ($l -match '^F \d+\s+ft=\s*([\d.]+)ms\s+ftr=(\d+) up=(\d+) wr=(\d+) fs=(\d+)') {
        $total++
        $ftr = [int]$Matches[2]; $up = [int]$Matches[3]
        $wr  = [int]$Matches[4]; $fs = [int]$Matches[5]
        $key = "ftr=$ftr up=$up wr=$wr fs=$fs"
        $combos[$key] = [int]$combos[$key] + 1

        if (Test-HardViolation $ftr $up $wr $fs $expect) { $hard++ }
        elseif ($fs -eq 0) { $soft++ }
    }
}

$noflip = @{}
foreach ($l in $lines) {
    if ($l -match 'no-flip: (.+)$') { $noflip[$Matches[1]] = [int]$noflip[$Matches[1]] + 1 }
}

Write-Host "`nFrame counter combinations observed:" -ForegroundColor Cyan
foreach ($e in ($combos.GetEnumerator() | Sort-Object Value -Descending)) {
    $null = $e.Key -match 'ftr=(\d+) up=(\d+) wr=(\d+) fs=(\d+)'
    $ftr = [int]$Matches[1]; $up = [int]$Matches[2]
    $wr  = [int]$Matches[3]; $fs = [int]$Matches[4]

    # Label from the same predicate the verdict uses, so the two can never
    # disagree - an earlier version pattern-matched strings and printed
    # VIOLATION alongside PASS.
    $kind =
        if (Test-HardViolation $ftr $up $wr $fs $expect) { 'VIOLATION - simulation or present ran twice' }
        elseif ($wr -gt 0 -and $fs -eq 1) { "world frame - expected ($wr render$(if($wr -ne 1){'s'}))" }
        elseif ($wr -eq 0 -and $fs -eq 1) { 'menu/loading frame - expected' }
        elseif ($wr -gt 0 -and $fs -eq 0) { 'world rendered, not presented - benign if explained below' }
        else                              { 'transition frame, nothing drawn or presented - benign if explained below' }

    Write-Host ("  {0,-28} x{1,-8} {2}" -f $e.Key, $e.Value, $kind)
}

if ($noflip.Count) {
    Write-Host "`nNo-flip reasons logged by the engine:" -ForegroundColor Cyan
    foreach ($e in ($noflip.GetEnumerator() | Sort-Object Value -Descending)) {
        Write-Host ("  {0,-8} {1}" -f "x$($e.Value)", $e.Key)
    }
}

$world = 0
foreach ($e in $combos.GetEnumerator()) { if ($e.Key -notmatch 'wr=0') { $world += $e.Value } }

Write-Host "`nTotals:" -ForegroundColor Cyan
Write-Host "  frames logged        : $total"
Write-Host "  world frames         : $world"
Write-Host "  violations           : $hard      <- must be 0"
Write-Host "  unpresented frames   : $soft"

Write-Host ""
if ($expect -lt 0) {
    Write-Host "NO VERDICT - mixed run, see above." -ForegroundColor Yellow
} elseif (-not $world) {
    Write-Host "INCONCLUSIVE - no world frames. Load a level and play before quitting." -ForegroundColor Yellow
} elseif ($hard -gt 0) {
    Write-Host "FAIL - $hard frames stepped something more than once. Paste this output." -ForegroundColor Red
} elseif ($soft -gt 0 -and $noflip.Count -eq 0) {
    Write-Host "INCONCLUSIVE - $soft unpresented frames with no reason logged." -ForegroundColor Yellow
} else {
    Write-Host "PASS - world drawn $expect x per frame, nothing else stepped twice, over $world world frames." -ForegroundColor Green
}

# --- host side --------------------------------------------------------------
# The client log says how fast the game DRAWS. It cannot say how many of those
# frames reached the headset - Windows Graphics Capture delivers a frame when
# the compositor presents the game window, at the refresh rate of the display
# that window is on. That gap was 34% of frames on a 60 Hz monitor and is
# invisible from the client side, so show it here rather than in a second
# command the player has to be handed each time.
$hostDir = Get-ChildItem (Join-Path (Split-Path $PSScriptRoot -Parent) 'logs') -Directory -EA SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($hostDir) {
    $hostLog = Join-Path $hostDir.FullName 'vrhost.log'
    if (Test-Path $hostLog) {
        Write-Host ""
        Write-Host "=== VR host: $($hostDir.Name) ===" -ForegroundColor Cyan
        # Broad on purpose. A narrow list has now hidden three different
        # results from the player - capture age, TRUE staleness, and the field
        # calibration - each time costing a run to discover the line existed.
        # Exclude the noisy per-format enumeration instead of listing what to keep.
        Select-String -Path $hostLog -Pattern 'swapchain format\[' -NotMatch |
        # No whitelist. A pattern list has now hidden FIVE separate results -
        # capture age, true staleness, the field calibration, its follow-up
        # line, and the controller tracking report - each costing a run to
        # discover the line had been there the whole time. Show everything
        # except the per-format enumeration, which is the only real noise.
        Select-String -Pattern 'swapchain format\[' -NotMatch |
            ForEach-Object { $_.Line } | ForEach-Object -Begin { $all = @() } -Process { $all += $_ } -End {
                # Startup AND the tail. Truncating to the last N hid the
                # controller report at 5.5 s behind ninety seconds of periodic
                # stats - the sixth time this script has concealed a result.
                if ($all.Count -le 45) { $all | ForEach-Object { "  " + $_ } }
                else {
                    $all[0..24] | ForEach-Object { "  " + $_ }
                    "  ... $($all.Count - 40) lines omitted ..."
                    $all[-15..-1] | ForEach-Object { "  " + $_ }
                }
            }
    }
}
