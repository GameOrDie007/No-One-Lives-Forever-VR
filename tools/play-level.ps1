# Put the headset straight into any level in the game.
#
# Testing a change used to mean playing to wherever the change lives. It does
# not have to: `+runworld` has been in NOLF since retail and loads a named
# world directly, skipping the menus, the mission list and the save system.
# This wraps it in a name you can actually remember.
#
#   .\tools\play-level.ps1 -List              every level, with its kind
#   .\tools\play-level.ps1 M06S01             load it
#   .\tools\play-level.ps1 M06S01 -At "-994,-100,854"    and stand there
#   .\tools\play-level.ps1 training           anything matching, listed
#   .\tools\play-level.ps1 M01S02 -Minutes 10 -Set @('VRViewModel=1')
#
# Matching is a case-insensitive substring against the world's path AND its
# kind, so "M06S01", "deathmatch" and "training" all work. More than one match
# prints the list rather than guessing - loading the wrong level and testing in
# it costs a whole headset round.

param(
    [Parameter(Position = 0)]
    [string]  $Level = '',
    [switch]  $List,
    [string]  $At = '',
    [string]  $Minutes = '60',
    [string[]]$Set = @()
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent

# THE MISSION NAMES ARE READ OUT OF THE GAME, NOT GUESSED.
#
# The first draft of this file labelled every world with a mission title and
# every one of those titles was invented, so it was replaced by what the PATH
# could prove: "campaign - mission 6, scene 1". That was honest and it was
# still not good enough - in a monospace column `M16S01`, a GOTY bonus
# mission, and `M06S01`, campaign mission 6, differ by one character, and
# the tester read one as the other on the first day the list existed.
#
# So the names now come from ATTRIBUTES/MISSIONS.TXT inside NOLFGOTY.REZ,
# which lists each mission with the worlds that make it up. tools/level-names.txt
# is generated from it by tools/mission-names.py and cross-checked against the
# mission names the game itself writes into Save/Save1001.ini.
#
# Anything the game does not name - multiplayer maps, the tech demo - still
# falls back to the kind derived from the path below.
$nameFile = Join-Path $Root 'tools\level-names.txt'
$names = @{}
if (Test-Path $nameFile) {
    foreach ($line in Get-Content $nameFile) {
        if ($line.StartsWith('#') -or -not $line.Contains("`t")) { continue }
        $bits = $line -split "`t", 2
        $names[$bits[0].Trim().ToLower()] = $bits[1].Trim()
    }
}

function Get-LevelKind($short) {
    if ($short -match 'MULTI.DEATHMATCH') { return 'multiplayer - deathmatch' }
    if ($short -match 'MULTI.ASSAULTMAP') { return 'multiplayer - assault' }
    if ($short -match 'MULTI')            { return 'multiplayer' }
    if ($short -match 'GOTY')             { return 'GOTY bonus mission' }
    if ($short -match 'EXTRAS')           { return 'engine demo' }
    if ($short -match 'T(\d\d)S(\d\d)$')  { return 'training' }
    if ($short -match 'M(\d\d)S(\d\d)$')  {
        return ('campaign - mission {0}, scene {1}' -f [int]$Matches[1], [int]$Matches[2])
    }
    return ''
}

# The game's own name where there is one; the path-derived kind where there is
# not. A GOTY bonus mission says so, because MISSIONS.TXT is what put it there.
function Get-LevelLabel($short) {
    $hit = $names[$short.ToLower()]
    if ($hit) {
        if ($short -match '\\GOTY\\') { return ('{0}   [GOTY bonus]' -f $hit) }
        return $hit
    }
    return (Get-LevelKind $short)
}

$worlds = Get-Content (Join-Path $Root 'tools\worlds.txt') |
          Where-Object { $_.Trim() }

function Get-Short($w) { ($w -replace '\.DAT$','') -replace '/','\' }

function Show-Levels($items) {
    foreach ($w in $items) {
        $short = Get-Short $w
        '{0,-42} {1}' -f $short, (Get-LevelLabel $short)
    }
}

if ($List -or -not $Level) {
    Write-Host ''
    Write-Host 'Every level in the game. Pass any part of a name or kind.' -ForegroundColor Cyan
    Write-Host ''
    Show-Levels $worlds | ForEach-Object { Write-Host "  $_" }
    Write-Host ''
    Write-Host '  .\tools\play-level.ps1 M06S01' -ForegroundColor Green
    Write-Host ''
    return
}

$hits = @($worlds | Where-Object {
    $short = Get-Short $_
    ($short -like "*$Level*") -or ((Get-LevelLabel $short) -like "*$Level*")
})

if ($hits.Count -eq 0) {
    Write-Host ("Nothing matches '{0}'. Try -List." -f $Level) -ForegroundColor Red
    return
}
if ($hits.Count -gt 1) {
    Write-Host ("'{0}' matches {1} levels:" -f $Level, $hits.Count) -ForegroundColor Yellow
    Show-Levels $hits | ForEach-Object { Write-Host "  $_" }
    Write-Host 'Be more specific.' -ForegroundColor Yellow
    return
}

# WORLDS/M06S01.DAT -> Worlds\M06S01, which is the form runworld wants.
$world = Get-Short $hits[0]
Write-Host ("loading {0}   ({1})" -f $world, (Get-LevelLabel $world)) -ForegroundColor Green

$fwd = @{ Native = $true; World = $world; Minutes = $Minutes }
if ($At)        { $fwd['At'] = $At }
if ($Set.Count) { $fwd['Set'] = $Set }

& (Join-Path $PSScriptRoot 'play-vr.ps1') @fwd
