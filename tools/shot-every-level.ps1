# A PICTURE OF EVERY LEVEL IN THE GAME.
#
# The level sweep (sweep-levels.ps1) answers questions with NUMBERS - polygon
# accounts, light grids, crashes - and kills each world the moment the account
# prints. That is the right instrument for "is any level dark" and the wrong one
# for "does this level LOOK right", which is the question raised in testing on
# 7 September after watching a sweep go past: missing sky, text in black boxes,
# flickering sections, none of which a counter reports.
#
# So: load every world, let it settle, photograph it, keep the renderer log
# beside the shot. 103 worlds, about half an hour, nobody in the room.
#
#   .\tools\shot-every-level.ps1
#   .\tools\shot-every-level.ps1 -Start 40 -Count 10
#   .\tools\shot-every-level.ps1 -Look "0,-15,0"      point the head down a bit
#
# Output: logs\shots\<WORLD>.png and logs\shots\<WORLD>-renstub.log

param(
    [string]  $List   = 'tools\worlds.txt',
    [string]  $OutDir = 'logs\shots',
    [int]     $Start  = 0,
    [int]     $Count  = 0,
    [int]     $Wait   = 16,
    [string]  $Look   = '0,0,0',
    # THE CAPTURE SIZE, AND WHY IT IS NOT look-shot's DEFAULT.
    #
    # On 20 September five captures in six came back solid black from row 720 -
    # half of the 1440 display - while the game's own log said it was rendering
    # 2560x1384. That is DPI virtualisation in the grab: Windows hands it a
    # half-size surface, so a full-size request gets 1280x720 of real pixels and
    # black for the rest. A 103-world sweep at the truncating size is 103
    # useless pictures and half an hour gone, which is the whole value of the
    # sweep. 1280x692 fits inside the virtualised surface and comes back clean.
    [int]     $ResW   = 1280,
    [int]     $ResH   = 692,
    [string[]]$Set    = @()
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Out  = Join-Path $Root $OutDir
New-Item -ItemType Directory -Force $Out | Out-Null

$worlds = Get-Content (Join-Path $Root $List) | Where-Object { $_.Trim() }
if ($Start -gt 0) { $worlds = $worlds | Select-Object -Skip $Start }
if ($Count -gt 0) { $worlds = $worlds | Select-Object -First $Count }

Write-Host ("photographing {0} worlds" -f @($worlds).Count) -ForegroundColor Cyan
$n = 0
foreach ($w in $worlds) {
    $n++
    # WORLDS/M01S01.DAT -> Worlds\M01S01, which is the form runworld wants.
    # Handed the list entry verbatim, +runworld accepts it, loads nothing, and
    # the game sits on its loading screen - which photographs perfectly well
    # and is how nine levels were "captured" before anyone looked at them.
    $short = ($w -replace '\.DAT$','') -replace '/','\'
    $name  = ($w -replace '.*[/\\]','') -replace '\.DAT$',''
    $png   = Join-Path $OutDir ($name + '.png')
    Write-Host ("[{0}/{1}] {2}" -f $n, @($worlds).Count, $name) -ForegroundColor Yellow

    # ONE FAILED LEVEL MUST NOT END THE RUN. A world that will not load is the
    # single most interesting result here and it is also the one that throws.
    try {
        $a = @('-File', (Join-Path $PSScriptRoot 'look-shot.ps1'),
               '-World', $short, '-Out', $png, '-Wait', "$Wait", '-Look', $Look,
               '-ResW', "$ResW", '-ResH', "$ResH")
        if ($Set.Count) { $a += @('-Set') + $Set }
        & powershell -NoProfile -ExecutionPolicy Bypass @a 2>&1 |
            ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkGray }
    }
    catch {
        Write-Host ("    FAILED: " + $_.Exception.Message) -ForegroundColor Red
    }
}
Write-Host "done" -ForegroundColor Green
