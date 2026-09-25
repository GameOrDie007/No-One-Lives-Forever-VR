# Photograph every weapon VRDebugWeapon can reach, the way the Sterling was
# settled: held flash, ONE resolution for all of them, fired just before the
# shutter so nothing runs dry and a tracer is still in the air.
#
# WHY ONE RESOLUTION MATTERS. The first revolver A/B was taken at 2560x1384 and
# 1280x692 because the retry fell back on one of them, and two frames at two
# scales cannot be compared by eye at all - I spent a round of measurement on
# pixel distances that meant nothing. 1280x692 survives DPI virtualisation, so
# ask for it up front and never retry into a different geometry.
#
# WHY IT FIRES EARLY, WHICH IS THE OPPOSITE OF WHAT THIS FILE FIRST SAID.
#
# The first version fired two seconds before the shutter, reasoning that holding
# the trigger from t+8 empties a revolver. It does - but firing late is worse,
# because several weapons load with an EMPTY CLIP and a full reserve (the AK47
# at 0/270, the Contender at 0/22, the Luger at 0/190), so a two-second window
# is spent reloading and the shutter catches an idle gun.
#
# That was a missing ReloadClip in the debug selector, not a timing problem, and
# it is fixed - the clip is now topped up every 0.7s for as long as the weapon is
# held, so it cannot run dry however long the trigger is down. Firing from t+8
# therefore gives the longest possible window with the flash held and a tracer in
# the air, which is what the shutter wants.
param(
    [int]    $First = 1,
    [int]    $Last  = 17,
    [int]    $Wait  = 22,
    [string] $Dir   = 'logs\wsweep',
    # Seconds in to squeeze the trigger. The clip is refilled while held, so
    # earlier simply means a longer burst before the shutter.
    [double] $FireAt = 8.0,
    [switch] $NoHold          # the energy guns and the delisle white out under the hold
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force (Join-Path $root $Dir) | Out-Null

for ($w = $First; $w -le $Last; $w++) {
    $tag = '{0:d2}' -f $w
    $out = Join-Path $Dir "w$tag.png"
    $set = @('VRCheats=2', "VRDebugWeapon=$w")
    if (-not $NoHold) { $set += 'VRFlashHold=1' }

    Write-Host ("[{0}/{1}] weapon selector {0}" -f $w, $Last) -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'look-shot.ps1') `
        -Out $out -Wait $Wait -ResW 1280 -ResH 692 `
        -RHandPos "-0.06,1.40,-0.50" `
        -Buttons 1 -ButtonsHand right -ButtonsAt $FireAt `
        -Set $set | Out-Null

    # The name the game actually chose - the offline ordering is a prediction,
    # and a weapon he does not own or that is out of ammo is skipped silently.
    $run = Get-ChildItem (Join-Path $root 'game\logs') -Directory |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $cl = Join-Path $run.FullName 'client.log'
    $nm = ''
    if (Test-Path $cl) {
        $line = Select-String -Path $cl -Pattern "VRDebugWeapon $w`: '" | Select-Object -First 1
        if ($line -and $line.Line -match "VRDebugWeapon $w`: '([^']+)'") { $nm = $Matches[1] }
    }
    if (-not $nm) { $nm = '(never switched)' }
    "$tag`t$nm" | Add-Content -Path (Join-Path $root "$Dir\names.tsv") -Encoding utf8
    Write-Host ("        -> {0}" -f $nm) -ForegroundColor Green
}
Write-Host "sweep done: $Dir" -ForegroundColor Green
