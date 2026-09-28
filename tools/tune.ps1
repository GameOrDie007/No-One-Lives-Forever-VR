# Launch straight into the in-headset tuner, holding one gun, in one mode.
#
#   .\tools\tune.ps1 -Weapon delisle -Mode casing
#
# Modes: flash, tracer, casing, angle, scale, grip, aim, lens, lenssize,
# and the arms (Show body): hand, body, offhand, offroll.
# Weapons by their tune-file name: p38, luger, walther_smg, ak47, sterling,
# delisle (the Hampton Carbine), dragunov, contender, ...
#
# While tuning: hold the LEFT GRIP; left stick left/right = back/forward
# along the barrel, down/up = down/up; keyboard Left/Right arrows = across
# the gun; left X = step size; left Y = reset this gun's values for the
# mode. Values save to game\vrtune.cfg as you change them.
param(
    [string]$Weapon = 'delisle',
    [ValidateSet('flash','tracer','casing','angle','scale','grip','aim','lens','lenssize','hand','body','offhand','offroll')]
    [string]$Mode = 'casing'
)
$modes = @{ flash = 0; tracer = 1; casing = 2; angle = 3; scale = 4; grip = 5; aim = 6; lens = 7; lenssize = 8; hand = 9; body = 10; offhand = 11; offroll = 12 }
$n = $modes[$Mode]
Write-Host ("TUNER: {0} mode, holding {1}" -f $Mode.ToUpper(), $Weapon) -ForegroundColor Green
& (Join-Path $PSScriptRoot 'play-vr.ps1') -Native -God -Mirror -Set @(
    'VRFlashTune=1',
    ('VRFlashTuneMode=' + $n),
    'VRDebugWeapon=1',
    ('VRDebugWeaponName=' + $Weapon)
)
