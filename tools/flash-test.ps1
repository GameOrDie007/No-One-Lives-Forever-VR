# ---------------------------------------------------------------------------
# flash-test.ps1 - FIRE A GUN AT THE DESK AND SEE WHETHER IT FLASHES.
#
# WHY THIS EXISTS, and why every earlier answer about the muzzle flash was
# worthless. The VRFlash diagnostic reported "PV flash shown 0 times" in run
# after run, and that was read as evidence that the flash was broken. It was
# not evidence about the flash at all:
#
#   * A level started with +runworld never leaves its opening camera. The
#     player camera sits in CHASE, and the view weapon is hidden AND DISABLED.
#     A disabled view weapon has no first-person flash to show, so the counter
#     was measuring the camera, not the effect.
#   * VRDebugEndCinematic was written to fix exactly that, and did not. It
#     called TurnOffAlternativeCamera, which is client-side, and
#     UpdateAlternativeCamera turned the camera straight back on the next frame
#     for any camera object still carrying USRFLG_CAMERA_LIVE. The switch
#     logged that it had fired - and it had - and it changed nothing for more
#     than one frame. It now clears that flag too.
#   * And nothing at the desk could pull a trigger. VRDebugFire now can.
#
# So this is the first arm that can actually answer the question, which is why
# it is a script and not a line in a transcript: the combination of switches is
# not guessable and the failure mode is a clean, confident, meaningless zero.
#
# THE CONTROL THAT CAN FAIL. -NoSilencer 0 runs the same arm with the silencer
# left alone. NOLF deliberately suppresses the flash for a silenced weapon, so
# that arm SHOULD report zero. If both arms report zero the switch is not the
# explanation; if both report flashes, the silencer suppression is broken.
#
#   .\tools\flash-test.ps1                    both arms, M01S01
#   .\tools\flash-test.ps1 -World Worlds\M02S01 -Weapon 3
# ---------------------------------------------------------------------------
param(
    [string]$World   = 'Worlds\M01S01',
    [int]   $Weapon  = 0,        # VRDebugWeapon <n>: 0 leaves the level's own choice
    [double]$FireAt  = 8.0,      # seconds in world before the trigger is held
    [int]   $Wait    = 40,
    [string]$Out     = 'logs\flash',
    [ValidateSet('both','on','off')]
    [string]$NoSilencer = 'both'
)

$ErrorActionPreference = 'Continue'
$Root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force (Join-Path $Root $Out) | Out-Null

$arms = @()
if ($NoSilencer -ne 'off') {
    $arms += @{ name = 'no-silencer'; sil = 1
                expect = 'flashes EXPECTED - an unsilenced weapon must flash' }
}
if ($NoSilencer -ne 'on') {
    $arms += @{ name = 'as-is'; sil = 0
                expect = 'ZERO expected if the level hands out a silenced gun - this is the control' }
}

$results = @()
foreach ($arm in $arms) {
    Write-Host ("=== {0} ===" -f $arm.name) -ForegroundColor Magenta
    Write-Host ("    {0}" -f $arm.expect) -ForegroundColor DarkGray

    $set = "VRDebugArsenal=1;VRDebugFire=$FireAt;VRDebugNoSilencer=$($arm.sil)"
    if ($Weapon -gt 0) { $set += ";VRDebugWeapon=$Weapon" }

    & (Join-Path $PSScriptRoot 'look-shot.ps1') `
        -World $World -Wait $Wait -Set $set `
        -Out "$Out\$($arm.name).png" | Out-Host

    # The newest run directory is this arm's.
    $dir = Get-ChildItem (Join-Path $Root 'game\logs') -Directory -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -match '^\d{8}-\d{6}$' } |
           Sort-Object Name -Descending | Select-Object -First 1
    if (-not $dir) { Write-Host '    no run directory' -ForegroundColor Red; continue }
    $log = Join-Path $dir.FullName 'client.log'
    if (-not (Test-Path $log)) { Write-Host '    no client.log' -ForegroundColor Red; continue }

    $last = Select-String -Path $log -Pattern '^\s*\[.*VRFlash:' -Encoding ascii |
            Select-Object -Last 1
    $fired = Select-String -Path $log -Pattern 'VRDebugFire: holding the trigger' -Encoding ascii
    $cine  = Select-String -Path $log -Pattern 'VRDebugEndCinematic:' -Encoding ascii

    $shown = -1
    if ($last -and $last.Line -match 'PV flash shown (\d+) times') { $shown = [int]$Matches[1] }

    Write-Host ("    cinematic ended : {0}" -f $(if ($cine) { 'yes' } else { 'NO  <- the camera is still CHASE, this arm proves nothing' }))
    Write-Host ("    trigger held    : {0}" -f $(if ($fired) { 'yes' } else { 'NO  <- nothing fired, this arm proves nothing' }))
    Write-Host ("    PV flash shown  : {0}" -f $shown)
    if ($last) { Write-Host ("    {0}" -f $last.Line.Trim()) -ForegroundColor DarkGray }

    $results += [pscustomobject]@{ arm = $arm.name; shown = $shown
                                   ended = [bool]$cine; fired = [bool]$fired }
}

Write-Host ''
Write-Host 'HOW TO READ IT' -ForegroundColor Cyan
Write-Host '  Both "cinematic ended" and "trigger held" must say yes, in BOTH arms.'
Write-Host '  A no in either one means the arm never reached the state being tested'
Write-Host '  and its zero is a harness result, not a game result - which is exactly'
Write-Host '  the mistake this script was written to stop repeating.'
Write-Host ''
$results | Format-Table -AutoSize | Out-Host
