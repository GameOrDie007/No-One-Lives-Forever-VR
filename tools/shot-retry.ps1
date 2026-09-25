# look-shot.ps1 with a retry when the capture comes back half black.
#
# WHY THIS EXISTS. Overnight on 20 September, five captures out of six came back
# 48% black, and every one was solid black from ROW 720 exactly - the game had
# come up in a 720-tall mode on a repeated launch while the harness captured the
# full 2560x1384 region. The numbers in the log were still good, but the PICTURE
# is the thing that matters here, and a half-black picture cannot be read.
#
# Losing four runs in five turns a three-minute check into a fifteen-minute one,
# which is the difference between verifying every weapon and verifying two.
#
# The test is not "how much of the frame is black" - a dark level is legitimately
# black - but "is there a run of rows at the BOTTOM where everything is black".
# A real frame does not do that.
#
#   .\tools\shot-retry.ps1 -Out logs\x.png -Wait 28 -RHand "35,0,0" `
#       -RHandPos "0.22,1.38,-0.38" -Set @('VRCheats=2','VRDebugWeapon=2')
#
# The arguments mirror look-shot.ps1's, which stays the single place that knows
# how to capture; this only decides whether to believe the result.

param(
    [int]      $Tries      = 3,
    [string]   $Out        = 'logs\retry.png',
    [int]      $Wait       = 28,
    [string]   $RHand      = '',
    [string]   $RHandPos   = '',
    [string]   $RHandSweep = '',
    [string]   $LHand      = '',
    [string]   $LHandPos   = '',
    [int]      $Buttons    = 0,
    [string]   $ButtonsHand= 'both',
    [double]   $ButtonsAt  = 6.0,
    [string[]] $Set        = @(),
    # THE FALLBACK SIZE, AND WHY IT WORKS. The truncation is DPI
    # virtualisation, not the game: the client log says "screen 2560x1384" on
    # exactly the runs whose capture is black from row 720, and 720 is half of
    # the 1440 display. Windows hands the capture a virtualised half-size
    # surface, so a 2560x1384 grab gets 1280x720 of real pixels and black for
    # the rest. Asking for 1280x692 in the first place fits inside that and
    # comes back clean - measured, 0.7% black against 48.2%.
    #
    # Retrying the SAME size does not help. It is not a race: three tries in a
    # row gave 664 dead rows each time.
    [int]      $FallbackW  = 1280,
    [int]      $FallbackH  = 692
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

function Test-FrameGood([string]$png) {
    if (!(Test-Path $png)) { return $false }
    $probe = @'
from PIL import Image
import numpy as np, sys
a = np.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=np.int16)
black = (a.max(axis=2) < 12)
rows = black.mean(axis=1)
h = len(rows)
dead = 0
for i in range(h - 1, -1, -1):
    if rows[i] > 0.98: dead += 1
    else: break
print("BAD" if dead > h * 0.10 else "OK", dead, "dead rows of", h)
'@
    $tmp = Join-Path $env:TEMP "framecheck.py"
    Set-Content -Path $tmp -Value $probe -Encoding utf8
    $res = (& python $tmp $png 2>&1) -join ' '
    if ($res -match '^BAD') {
        Write-Host ("    truncated frame: " + $res) -ForegroundColor Yellow
        return $false
    }
    return $true
}

for ($i = 1; $i -le $Tries; $i++) {
    $a = @{ Wait = $Wait; Out = $Out }
    # First try at whatever look-shot defaults to; every try after that at the
    # size that is known to survive DPI virtualisation.
    if ($i -gt 1) { $a['ResW'] = $FallbackW; $a['ResH'] = $FallbackH }
    if ($RHand)      { $a['RHand']      = $RHand }
    if ($RHandPos)   { $a['RHandPos']   = $RHandPos }
    if ($RHandSweep) { $a['RHandSweep'] = $RHandSweep }
    if ($LHand)      { $a['LHand']      = $LHand }
    if ($LHandPos)   { $a['LHandPos']   = $LHandPos }
    if ($Buttons)    { $a['Buttons']    = $Buttons; $a['ButtonsHand'] = $ButtonsHand; $a['ButtonsAt'] = $ButtonsAt }
    if ($Set.Count)  { $a['Set']        = $Set }

    & (Join-Path $PSScriptRoot 'look-shot.ps1') @a | Out-Null

    $full = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
    if (Test-FrameGood $full) {
        Write-Host ("    capture ok on try ${i}: $Out") -ForegroundColor Green
        exit 0
    }
    Write-Host ("    try $i of ${Tries}: truncated, retrying at ${FallbackW}x${FallbackH}") -ForegroundColor Yellow
}

Write-Host ("    STILL TRUNCATED after ${Tries} tries: $Out") -ForegroundColor Red
exit 1
