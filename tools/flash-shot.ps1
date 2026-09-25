# ---------------------------------------------------------------------------
# flash-shot.ps1 - FIRE THE GUN AT THE DESK and photograph the muzzle flash,
# with and without the fix, so the picture can fail.
#
# WHY THIS EXISTS. the muzzle flash was not
# attached to the gun. NOLF's first-person flash is a scale FX - an OT_MODEL
# carrying FLAG_REALLYCLOSE - and LithTech keeps such an object's position in
# CAMERA SPACE, so GetObjectPos hands back a point a few units from the map
# origin wherever the player is standing. The view weapon converts its own; the
# flash was never offered to the same conversion.
#
# The fix could not be checked without a headset, because nothing at the desk
# had ever pulled a trigger. It can: fakehost.py takes --buttons, the right
# trigger is a HELD command (slot 9 in the VR control map), and VRCheats 7
# hands over the arsenal. So the gun fires, the flash exists, and a screenshot
# settles it.
#
# TWO ARMS, because a flash photographed at the muzzle proves nothing on its
# own - it might have been there all along. VRCloseRebase 0 is the arm that is
# allowed to fail, and in it the flash should be somewhere else entirely.
#
#   .\tools\flash-shot.ps1
#   .\tools\flash-shot.ps1 -World "Worlds\M01S02" -Wait 20
# ---------------------------------------------------------------------------
param(
    # THE QUICK SAVE, NOT +runworld, AND THAT MATTERS MORE THAN IT SOUNDS.
    #
    # A level started with +runworld leaves its opening camera LIVE: the client
    # sits in the alternative-camera state for as long as the level runs, which
    # puts the player camera in CHASE, hides the view weapon and DISABLES the
    # weapon model. CWeaponModel::UpdateFlash returns immediately when the
    # weapon object is not FLAG_VISIBLE, so in that state there is no muzzle
    # flash and there never can be - measured on 11 September, M01S01 and
    # M01S02, at 3 seconds and at 70. Loaded from a save the same build reports
    # 'visible yes, camera first person, ext cam no'.
    #
    # So an empty -World means the quick save, which is the only entry into a
    # world this harness has that reproduces the state a player is in.
    [string]$World = '',
    # THE PLAYER STARTS WITH HIS FISTS, and fists have no muzzle flash. The
    # first run of this script fired for ten seconds with 'fisty_cuffs' in hand
    # and photographed nothing, twice, which is exactly the shape of a null
    # result that means the instrument never ran. VRCheats hands over the
    # arsenal but does not select from it, so a weapon key does.
    [string]$WeaponKey = '2',
    [int]   $Wait  = 9,           # seconds before the key
    [int]   $KeyDelay = 8,        # seconds between the key and the photograph
    [double]$FireAt = 12.0,       # the trigger goes down between the two
    [string]$Out   = 'logs\flash',
    [int]   $ResW  = 2560,
    [int]   $ResH  = 1384
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$OutDirAbs = Join-Path $Root $Out
New-Item -ItemType Directory -Force $OutDirAbs | Out-Null

# THE HAND POINTS SOMEWHERE THE HEAD DOES NOT. With both at (0,0,0) a gun
# placed by the HAND and one placed by the VIEW come out in the same place, and
# the whole question - is the flash on the gun or in front of the face - cannot
# be told apart. 25 degrees right of the view is enough to separate them in the
# picture and still keep the gun on screen.
$rhand = '25,0,0'

foreach ($arm in @(
    @{ name = 'rebase-on';  set = 'VRCheats=7;VRCloseRebase=1;VRDebugWeapon=1' },
    @{ name = 'rebase-off'; set = 'VRCheats=7;VRCloseRebase=0;VRDebugWeapon=1' }))
{
    # RELATIVE TO THE ROOT, because look-shot.ps1 joins what it is given onto
    # the project root - an absolute path there becomes E:\...\E:\... and the
    # only symptom is "The given path's format is not supported".
    $png = Join-Path $Out ("{0}.png" -f $arm.name)
    Write-Host ("=== {0} ===" -f $arm.name) -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'look-shot.ps1') `
        -Look '0,0,0' -RHand $rhand -World $World -Wait $Wait `
        -Keys $WeaponKey -KeyDelay $KeyDelay `
        -Buttons 1 -ButtonsHand right -ButtonsAt $FireAt `
        -Set $arm.set -Out $png -ResW $ResW -ResH $ResH

    # WHAT THE CLIENT SAID IT DID. The log line carries the before position,
    # the after position and the gun's own position, which is the claim and its
    # check together - see the VRClose block in GameClientShell.cpp.
    $d = Get-ChildItem (Join-Path $Root 'game\logs') -Directory -ErrorAction SilentlyContinue |
         Sort-Object Name | Select-Object -Last 1
    if ($d) {
        $cl = Join-Path $d.FullName 'client.log'
        Copy-Item $cl (Join-Path $OutDirAbs ("{0}-client.log" -f $arm.name)) -Force -ErrorAction SilentlyContinue
        $lines = Get-Content $cl -ErrorAction SilentlyContinue
        # WHICH WEAPON WAS IN HAND, first - every other number here is
        # meaningless if the answer is fists.
        $wep = $lines | Select-String "weapon '" | Select-Object -Last 1
        if ($wep) { Write-Host ('   ' + $wep.Line.Trim()) -ForegroundColor Cyan }
        if (($lines | Select-String 'VRControls: first fire of command 8').Count -eq 0) {
            Write-Host '   THE FIRE COMMAND NEVER FIRED - this run measured nothing' -ForegroundColor Red
        }
        $close = $lines | Select-String 'VRClose:'
        if ($close) { $close | Select-Object -Last 4 | ForEach-Object { '   ' + $_.Line.Trim() } }
        else        { Write-Host '   no VRClose line at all' -ForegroundColor Yellow }
    }
}
Write-Host ("pictures in {0}" -f $OutDirAbs) -ForegroundColor Green
