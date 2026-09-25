# ---------------------------------------------------------------------------
# make-quicksave.ps1 - PUT THE QUICK SAVE WHERE THE TEST NEEDS TO START.
#
# Asked for on 19 September: when a test needs a specific level, and ideally a
# specific spot in it, put the quick save there, because that would speed up
# testing a great deal.
#
# It is also the cheapest thing on the whole list. Every desk
# capture taken without -World loads game\Save\Quick.sav, so whatever that file
# holds is where EVERY test begins. On 19 September it held the HQ lobby beside
# the statue, and the consequences ran all day: no capture could find the
# waterfall under discussion, reaching a level with a gun in it needed the
# arsenal cheat, and a whole afternoon of "which level is that in" was spent on
# a question this script answers in one run.
#
#   .\tools\make-quicksave.ps1 -World "Worlds\M10S01"
#   .\tools\make-quicksave.ps1 -World "Worlds\T01S02" -At "-28,-70,1088" -Look "-3,-7.5,0"
#   .\tools\make-quicksave.ps1 -Restore          put the original save back
#
# THE EXISTING SAVE IS ALWAYS BACKED UP FIRST, to Save\Quick-before-<stamp>.sav, and
# -Restore puts the most recent one back. Overwriting the quick save is exactly
# the kind of silent, annoying loss this project has already had once, when
# a 12:41 save replaced the waterfall viewpoint the the development notes was still telling
# people to use.
#
# THE OPENING CINEMATIC HAS TO BE ENDED OR THE SAVE IS USELESS. A level entered
# with +runworld keeps its opening camera LIVE: the player camera sits in CHASE,
# the view weapon is hidden and disabled, and -At moves the PLAYER while the
# camera stays where the cinematic put it. VRDebugEndCinematic clears it. That
# trap cost several captures today before it was spotted, so it is on by
# default here rather than left to be remembered.
# ---------------------------------------------------------------------------
param(
    [string] $World   = '',                 # e.g. Worlds\M10S01. Empty = current quick save's level
    [string] $At      = '',                 # "x,y,z" to stand somewhere specific
    [string] $Look    = '0,0,0',            # yaw,pitch,roll for the saved facing
    [double] $SaveAt  = 14.0,               # seconds in-world before saving
    [int]    $Wait    = 26,                 # seconds to let the world settle
    [switch] $Restore,                      # put the original quick save back
    [switch] $NoVerify                      # skip the load-it-back check
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Save = Join-Path $Root 'game\Save'
$Quick = Join-Path $Save 'Quick.sav'

# A QUICK SAVE IS TWO FILES, NOT ONE, and finding that out the hard way is why
# this comment exists. Quick.sav holds the state; Save1001.ini holds the slot
# metadata, and its SaveGame00 line is where the game reads WHICH WORLD to load.
# Restoring only the .sav leaves the .ini pointing at the level the tool last
# made, so the game loads that world with the original save's contents - which on
# 19 September meant a restored HQ save booting into Morocco. Both, always.
$Ini = Join-Path $Save 'Save1001.ini'

function Latest-Backup {
    param([string]$Pattern)
    Get-ChildItem $Save -Filter $Pattern -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

if ($Restore) {
    $b = Latest-Backup 'Quick-before-*.sav'
    if (-not $b) { throw "no Quick-before-*.sav to restore from." }
    Copy-Item $b.FullName $Quick -Force
    Write-Host ("restored {0} -> Quick.sav" -f $b.Name) -ForegroundColor Green
    $bi = Latest-Backup 'Save1001-before-*.ini'
    if ($bi) {
        Copy-Item $bi.FullName $Ini -Force
        Write-Host ("restored {0} -> Save1001.ini (the world the slot points at)" -f $bi.Name) -ForegroundColor Green
    } else {
        Write-Host "  NO .ini backup found - the slot may still point at the level this tool made." -ForegroundColor Yellow
        Write-Host "  Check SaveGame00 in game\Save\Save1001.ini before trusting the quick save." -ForegroundColor Yellow
    }
    exit 0
}

if (-not $World) { throw "give -World, e.g. -World 'Worlds\M10S01'." }

# ---- back the existing save up first, always ---------------------------------------
if (Test-Path $Quick) {
    $stamp = (Get-Item $Quick).LastWriteTime.ToString('yyyyMMdd-HHmmss')
    $bk = Join-Path $Save "Quick-before-$stamp.sav"
    if (-not (Test-Path $bk)) { Copy-Item $Quick $bk -Force }
    Write-Host ("backed up the existing Quick.sav -> {0}" -f (Split-Path $bk -Leaf)) -ForegroundColor DarkGray
    $before = (Get-Item $Quick).LastWriteTime
} else { $before = [datetime]'2000-01-01' }

# ...and the slot metadata with it. See the note above: without this the
# restore puts the original save back under the wrong world name.
if (Test-Path $Ini) {
    $istamp = (Get-Item $Ini).LastWriteTime.ToString('yyyyMMdd-HHmmss')
    $ibk = Join-Path $Save "Save1001-before-$istamp.ini"
    if (-not (Test-Path $ibk)) { Copy-Item $Ini $ibk -Force }
    Write-Host ("backed up Save1001.ini -> {0}" -f (Split-Path $ibk -Leaf)) -ForegroundColor DarkGray
}

# ---- make the save ---------------------------------------------------------
$a = @('-File', (Join-Path $PSScriptRoot 'look-shot.ps1'),
       '-World', $World, '-Look', $Look, '-Wait', "$Wait",
       '-Out', 'logs\quicksave-made.png',
       '-Set', "VRDebugEndCinematic=4,VRDebugQuickSave=$SaveAt")
if ($At) { $a += @('-At', $At) }

Write-Host ("making a quick save in {0}{1}..." -f $World, $(if ($At) { " at $At" } else { '' })) -ForegroundColor Cyan
& powershell -NoProfile -ExecutionPolicy Bypass @a 2>&1 |
    ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkGray }

# ---- did it actually write one? -------------------------------------------
if (-not (Test-Path $Quick)) { throw "no Quick.sav was written." }
$after = (Get-Item $Quick).LastWriteTime
if ($after -le $before) {
    throw ("Quick.sav was NOT rewritten (still {0}). The save did not happen - " +
           "check the client log for 'VRDebugQuickSave'.") -f $after
}
Write-Host ("Quick.sav rewritten at {0}" -f $after.ToString('HH:mm:ss')) -ForegroundColor Green

# ---- and does it load back into the level asked for? ----------------------
#
# The save reporting success is not the same claim as the save being loadable
# into the right world, and only the second one is useful. So load it.
if ($NoVerify) { exit 0 }

Write-Host "verifying: loading the new quick save back..." -ForegroundColor Cyan
& powershell -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $PSScriptRoot 'look-shot.ps1') `
    -Wait 24 -Out 'logs\quicksave-verify.png' 2>&1 |
    ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkGray }

$ren = Join-Path $Root 'game\logs\renstub.log'
$want = ($World -replace '.*[\\/]', '').ToUpper()
$got = (Select-String -Path $ren -Pattern 'WORLD FILE: (\S+)' |
        Select-Object -Last 1).Matches.Groups[1].Value
if ($got -and $got.ToUpper().Contains($want)) {
    Write-Host ("VERIFIED: the quick save loads into {0}" -f $got) -ForegroundColor Green
} else {
    Write-Host ("FAILED: the quick save loaded '{0}', wanted something containing '{1}'" -f $got, $want) -ForegroundColor Red
    Write-Host "  the original save is still in Save\Quick-before-*.sav; -Restore puts it back" -ForegroundColor Yellow
    exit 1
}
