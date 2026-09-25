# What muzzle offset does each weapon ACTUALLY use? Measured, not derived.
#
# Three attempts to compute these offline have now failed - .abc mesh extents,
# a millimetre comparison against the real firearms, and a relative comparison
# against a verified weapon. All three died on the same rock: the player-view
# models share no coordinate convention, so there is no offline rule. See
# the development notes.
#
# So stop deriving and start recording. VRFlashTrace prints the flash's offset
# from the drawn gun decomposed in the GUN'S frame, and the flash and the tracer
# were confirmed to share that point (Luger 59.57 vs 60, Sterling 50.85 vs 51),
# so the 'f' figure is the muzzle distance for that weapon, full stop.
param(
    [int]    $First = 1,
    [int]    $Last  = 17,
    [int]    $Wait  = 24,
    [string] $Out   = 'logs\muzzle-table.tsv'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dest = Join-Path $root $Out
"selector`tweapon`tright`tup`tforward`tlen" | Set-Content $dest -Encoding utf8

for ($w = $First; $w -le $Last; $w++) {
    & (Join-Path $PSScriptRoot 'look-shot.ps1') `
        -Out ('logs\wsweep\mt{0:d2}.png' -f $w) -Wait $Wait -ResW 1280 -ResH 692 `
        -RHandPos "-0.06,1.40,-0.50" `
        -Buttons 1 -ButtonsHand right -ButtonsAt 8 `
        -Set @('VRCheats=2', "VRDebugWeapon=$w", 'VRFlashTrace=1') | Out-Null

    $run = Get-ChildItem (Join-Path $root 'game\logs') -Directory |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $cl = Join-Path $run.FullName 'client.log'
    $nm = '(already held)'
    $m  = Select-String -Path $cl -Pattern "VRDebugWeapon $w`: '" | Select-Object -First 1
    if ($m -and $m.Line -match "VRDebugWeapon $w`: '([^']+)'") { $nm = $Matches[1] }

    $ft = Select-String -Path $cl -Pattern 'VRFlashTrace' | Select-Object -First 1
    if ($ft -and $ft.Line -match 'r\s+([-+0-9.]+) u\s+([-+0-9.]+) f\s+([-+0-9.]+) \| len\s+([0-9.]+)') {
        $row = "{0}`t{1}`t{2}`t{3}`t{4}`t{5}" -f $w, $nm, $Matches[1], $Matches[2], $Matches[3], $Matches[4]
        Write-Host ("  {0,2}  {1,-22} forward {2,7}  len {3}" -f $w, $nm, $Matches[3], $Matches[4]) -ForegroundColor Green
    } else {
        $row = "{0}`t{1}`t`t`t`t(no flash - never fired)" -f $w, $nm
        Write-Host ("  {0,2}  {1,-22} no flash logged" -f $w, $nm) -ForegroundColor Yellow
    }
    $row | Add-Content $dest -Encoding utf8
}
Write-Host "wrote $Out" -ForegroundColor Green
