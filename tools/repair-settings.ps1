# ---------------------------------------------------------------------------
# repair-settings.ps1 - switch on the game features a staged install never
# configured.
#
# WHY THIS EXISTS. CGameSettings::GetBoolVar returns FALSE when the console
# variable does not EXIST, and a NOLF install whose Options pages have never
# been visited simply has no entry for most of them. So the game runs with the
# feature switched off and nothing anywhere says so. Measured on a test
# install, 11 September 2026, by logging every setting at world entry
# (VRSettings in the client log):
#
#   Gore=ABSENT PolyGrids=ABSENT DrawSky=ABSENT PVWeapons=ABSENT
#   ScreenFlash=ABSENT TextureDetail=ABSENT ModelLOD=ABSENT
#   ModelFullbrite=ABSENT CloudMapLight=ABSENT SpecialFX=ABSENT ...
#
# What that cost, as reported from the headset: the waterfall did not move -
# PolyGridFX skips its ENTIRE wave update when PolyGrids is off, and halves the
# grid again when PerformanceLevel is below 2. There was no blood splatter on
# walls - CWeaponFX::CreateSurfaceSpecificFX returns early when Gore is off.
# Neither is a renderer bug and no amount of looking at the renderer would
# have found either.
#
# THE VALUES ARE NOT INVENTED. Every one is read out of a REAL RETAIL NOLF
# install's own saved configuration (the retail autoexec.cfg, written by
# the retail game itself), so this restores what the original game runs with
# rather than what seemed sensible.
#
# It only ADDS. A setting already present is left exactly as it is, whatever
# its value, because that is the player's choice - including a player who
# turned gore off on purpose.
#
#   .\tools\repair-settings.ps1                    # the staging folder
#   .\tools\repair-settings.ps1 -Config <path>     # any autoexec.cfg
#   .\tools\repair-settings.ps1 -WhatIf            # say, do not write
# ---------------------------------------------------------------------------
param(
    [string]$Config = '',
    [switch]$WhatIf,
    # RAISE THE QUALITY PRESETS TOO. Separate from the add-only repair above,
    # and OFF by default, because these settings EXIST and an existing value
    # is the player's business.
    #
    # A test install had all three at 1 (medium) - the shape of an auto-detect
    # run once on first launch, not a choice anybody made. What that costs:
    # PolyGridFX HALVES every water grid below PerformanceLevel 2, and the
    # impact and debris levels thin out what you see when you shoot something.
    [switch]$Quality
)

$ErrorActionPreference = 'Stop'
if (-not $Config) {
    $Config = [System.IO.Path]::Combine((Split-Path $PSScriptRoot -Parent), 'game', 'autoexec.cfg')
}

# name -> value, as retail saves them. Ordered for a readable report.
$Want = [ordered]@{
    # --- features that are simply OFF when absent -------------------------
    'Gore'             = '1'          # blood on flesh hits (CWeaponFX)
    'PolyGrids'        = '1.000000'   # water surfaces animate at all (PolyGridFX)
    'DrawSky'          = '1.000000'   # the engine draws the sky
    'ScreenFlash'      = '1.000000'   # damage/pickup flash
    'CloudMapLight'    = '1.000000'
    'ModelFullbrite'   = '1'
    'DynamicLight'     = '1.000000'
    # --- quality levels that read as 0 (= lowest) when absent --------------
    'TextureDetail'    = '2.000000'   # 2 is retail's high
    'ModelLOD'         = '2.000000'
    'SpecialFX'        = '2.000000'
    # --- present but lower than retail; only set if absent -----------------
    'PerformanceLevel' = '2'          # below 2, PolyGridFX HALVES every grid
    'BulletHoles'      = '300.000000'
    'Tracers'          = '1'
    'ShellCasings'     = '1'
    'LightMap'         = '1'
    'EnvMapEnable'     = '1'
    'DetailTextures'   = '1'
    'soundenable'      = '1'
    'musicenable'      = '1'
}

if (-not (Test-Path -LiteralPath $Config)) {
    Write-Host ("no config at {0} - the game writes one on its first exit; run the game once, then this." -f $Config) -ForegroundColor Yellow
    exit 0
}

$raw   = [System.IO.File]::ReadAllText($Config)
$nl    = if ($raw -match "`r`n") { "`r`n" } else { "`n" }

# THE ONE KEY THAT IS REMOVED, not added. "musictype" "ima" (compressed music,
# which a retail install can carry from its launcher) makes the engine's sound
# library crash on every focus change - see play-vr.ps1, which also removes it
# at each launch. Absent, the engine plays its default music.
if ($raw -match '(?im)^\s*"musictype"\s+"[^"]*"\s*$') {
    Write-Host 'removing "musictype" (compressed music crashes the game on focus changes)' -ForegroundColor Yellow
    if (-not $WhatIf) {
        $bak0 = $Config + '.before-repair'
        if (-not (Test-Path -LiteralPath $bak0)) { Copy-Item -LiteralPath $Config -Destination $bak0 -Force }
        $raw = [regex]::Replace($raw, '(?im)^\s*"musictype"\s+"[^"]*"\s*\r?\n?', '')
        [System.IO.File]::WriteAllText($Config, $raw, (New-Object System.Text.ASCIIEncoding))
    }
}
# THE VR DEFAULTS, ONCE PER CONFIG. A player's copied retail settings are
# their business - but a retail install is usually on "Normal quality" with
# 16-bit textures, and head bob and weapon sway move the view or the gun on
# their own, which is wrong in a headset. So the first time this runs on a
# config it sets Options > Performance to its HIGH QUALITY column (every
# setting that page compares, so the page reads "High quality" and not
# "Customized"; the sound filters are an audio choice and are left alone),
# 32-bit textures, and head bob and weapon sway off - then writes
# VRSetupDefaults so it never does it again, whatever the player changes.
# The renderer draws the world's reflections itself and has no mirrors to
# enable, so what this changes in play is more impact and debris effects and
# the detailed models kept at a distance (GroupOffset 0).
if ($raw -notmatch '(?im)^\s*"VRSetupDefaults"\s+"') {
    $bits32 = ($raw -match '(?im)^\s*"BitDepth"\s+"32')
    $VRDefaults = [ordered]@{
        'PerformanceLevel' = '2'
        'TripleBuffer'     = '1'
        'DrawPortals'      = $(if ($bits32) { '1' } else { '0' })	# mirrors, only at 32-bit (as the page does)
        'LightMap'         = '1'
        'DrawShadows'      = '1'
        'MaxModelShadows'  = '1'
        'DetailTextures'   = '1'
        'EnvMapWorld'      = '1'
        'EnvMapEnable'     = '1'
        'Trilinear'        = '1'
        'MuzzleLight'      = '1'
        'Tracers'          = '1'
        'ShellCasings'     = '1'
        'EnableWeatherFX'  = '1'
        'ImpactFXLevel'    = '2'
        'DebrisFXLevel'    = '2'
        'GroupOffset1'     = '0'
        'GroupOffset2'     = '0'
        'GroupOffset3'     = '0'
        'GroupOffset4'     = '0'
        'GroupOffset5'     = '0'
        'GroupOffset6'     = '0'
        '32BitTextures'    = '1'
        'HeadBob'          = '0.000000'
        'WeaponSway'       = '0.000000'
        'VRComfortDefaults' = '1'	# the client's own once-only switch for the two above
    }
    Write-Host 'first run on this config: VR defaults (High quality, 32-bit textures, head bob and weapon sway off)' -ForegroundColor Yellow
    if (-not $WhatIf) {
        $bak0 = $Config + '.before-repair'
        if (-not (Test-Path -LiteralPath $bak0)) { Copy-Item -LiteralPath $Config -Destination $bak0 -Force }
        foreach ($k in $VRDefaults.Keys) {
            $line = '"{0}" "{1}"' -f $k, $VRDefaults[$k]
            $pat  = '(?im)^\s*"' + [regex]::Escape($k) + '"\s+"[^"]*"\s*$'
            if ($raw -match $pat) { $raw = [regex]::Replace($raw, $pat, $line) }
            else {
                if ($raw.Length -gt 0 -and -not $raw.EndsWith($nl)) { $raw += $nl }
                $raw += $line + $nl
            }
        }
        if (-not $raw.EndsWith($nl)) { $raw += $nl }
        $raw += '"VRSetupDefaults" "1"' + $nl
        [System.IO.File]::WriteAllText($Config, $raw, (New-Object System.Text.ASCIIEncoding))
    }
}

$lines = $raw -split "`r?`n"

# Which names already have a line? The file is `"Name" "Value"`, and the
# engine is not case-sensitive about them - retail writes "performancelevel"
# and "Detailtextures" in lower case, so a case-sensitive test would add a
# SECOND entry and the last one read would win. Compare folded.
$have = @{}
foreach ($l in $lines) {
    if ($l -match '^\s*"([^"]+)"\s+"') { $have[$Matches[1].ToLower()] = $true }
}

$add = @()
foreach ($k in $Want.Keys) {
    if (-not $have.ContainsKey($k.ToLower())) { $add += , @($k, $Want[$k]) }
}

Write-Host ("config: {0}" -f $Config)
if (-not $add.Count) {
    Write-Host "every setting this game needs is already present." -ForegroundColor Green
    if (-not $Quality) { exit 0 }
}

if ($add.Count) {
    Write-Host ("{0} settings are ABSENT, so the game reads them as zero:" -f $add.Count) -ForegroundColor Yellow
    foreach ($p in $add) { Write-Host ("   {0,-18} -> {1}" -f $p[0], $p[1]) }
}

# The quality presets, raised rather than added. Retail's own values.
$raise = @()
if ($Quality) {
    $Want2 = [ordered]@{
        'PerformanceLevel' = '2'   # below 2, PolyGridFX halves every water grid
        'ImpactFXLevel'    = '2'
        'DebrisFXLevel'    = '2'
    }
    foreach ($k in $Want2.Keys) {
        $cur = $null
        foreach ($l in $lines) {
            if ($l -match ('^\s*"(?i)' + [regex]::Escape($k) + '"\s+"([^"]*)"')) { $cur = $Matches[1]; break }
        }
        if ($cur -ne $null) {
            $a = 0.0; $b = 0.0
            if ([double]::TryParse($cur, [ref]$a) -and [double]::TryParse($Want2[$k], [ref]$b) -and $a -lt $b) {
                $raise += , @($k, $cur, $Want2[$k])
            }
        }
    }
    if ($raise.Count) {
        Write-Host ''
        Write-Host ("{0} quality presets are BELOW retail's:" -f $raise.Count) -ForegroundColor Yellow
        foreach ($r in $raise) { Write-Host ("   {0,-18} {1} -> {2}" -f $r[0], $r[1], $r[2]) }
    }
}

if ($WhatIf) { Write-Host '(-WhatIf: nothing written)' -ForegroundColor Cyan; exit 0 }

# A BACKUP FIRST. This file holds the player's key bindings and every option
# they have ever set; it is not ours to risk.
$bak = $Config + '.before-repair'
if (-not (Test-Path -LiteralPath $bak)) { Copy-Item -LiteralPath $Config -Destination $bak -Force }

# Rewrite raised lines in place, so there is never a second entry for a name.
$text = $raw
foreach ($r in $raise) {
    $text = [regex]::Replace($text,
        ('(?im)^\s*"' + [regex]::Escape($r[0]) + '"\s+"[^"]*"\s*$'),
        ('"{0}" "{1}"' -f $r[0], $r[2]))
}
$raw = $text

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append($raw)
if ($raw.Length -gt 0 -and -not $raw.EndsWith($nl)) { [void]$sb.Append($nl) }
foreach ($p in $add) { [void]$sb.Append(('"{0}" "{1}"{2}' -f $p[0], $p[1], $nl)) }
[System.IO.File]::WriteAllText($Config, $sb.ToString(), (New-Object System.Text.ASCIIEncoding))

if (-not $add.Count -and -not $raise.Count) {
    Write-Host 'nothing to change.' -ForegroundColor Green
    exit 0
}
Write-Host ("written. a backup of the original is at {0}" -f $bak) -ForegroundColor Green
exit 0
