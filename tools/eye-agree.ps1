# ---------------------------------------------------------------------------
# eye-agree.ps1 - DO THE TWO EYES AGREE WHEN THEY ARE AT THE SAME POINT?
#
# With the fake host reporting an IPD of zero and the frustum symmetric, the two
# halves of a side-by-side capture are two renders of ONE camera. Every pixel
# that differs is a fault in our own pass - something sampled, seeded or clocked
# per PASS instead of per FRAME - and not stereo.
#
# That is how the per-eye animation clock was found: each eye asked the wall
# clock for the time, so an animated surface stood at two different moments in
# the two halves, which is what shimmer IS in a headset. In headset testing on
# 11 September the canopy and the water pool in Morocco both flickered. The
# clock was fixed; nothing has shown that the fix holds. This shows it, and it
# can fail.
#
# The frame marker is drawn per eye on purpose and always differs - a few
# hundred pixels in one corner, which is the 0.1-0.2% floor to expect.
#
#   .\tools\eye-agree.ps1                          the quick save
#   .\tools\eye-agree.ps1 -World "Worlds\M01S02"   Morocco, where the pool is
#   .\tools\eye-agree.ps1 -Sweep                   a MOVING head, the harder case
# ---------------------------------------------------------------------------
param(
    [string]$World = '',
    [string]$Look  = '0,0,0',
    [int]   $Wait  = 20,
    [string]$Out   = 'logs\eyeagree',
    [switch]$Sweep,
    [int]   $Thresh = 8
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force (Join-Path $Root $Out) | Out-Null

# TWO CAPTURES A FEW SECONDS APART, not one.
#
# A single frame cannot tell a per-pass fault from a still scene: if nothing in
# view is animating, the halves agree whatever the clock does. Two captures at
# different moments, with an animated surface in shot, is the test that can
# actually fail - and the difference between the two captures is itself the
# proof that something was moving.
$shots = @()
foreach ($n in 1, 2) {
    $png = Join-Path $Out ("eye{0}.png" -f $n)
    # BOTH SIDES HAVE TO AGREE ON ZERO. The fake host's --ipd sets what the
    # HOST reports; the client still applies its own VRIPD unless that is
    # zeroed too, and with VRIPDAuto on it may take the host's value back.
    # Without all three the halves differ by ordinary parallax - 48% of pixels
    # - and the test measures nothing. (A comment between two backtick
    # continuations also breaks the call, which is how this line lost its -Ipd.)
    & (Join-Path $PSScriptRoot 'look-shot.ps1') `
        -Look $Look -World $World -Wait ($Wait + ($n - 1) * 6) `
        -Ipd 0 -Set 'StubNativeFrustum=0;VRIPD=0;VRIPDAuto=0' -Sweep:$Sweep `
        -Out $png | Out-Host
    $shots += (Join-Path $Root $png)
}

Write-Host ''
Write-Host 'agreement between the two halves of each capture:' -ForegroundColor Cyan
& python (Join-Path $PSScriptRoot 'eye-identity.py') $shots[0] $shots[1] --thresh $Thresh |
    ForEach-Object { '  ' + $_ }

Write-Host ''
Write-Host 'and how much the SCENE moved between the two captures' -ForegroundColor Cyan
Write-Host '(if this is near zero the scene was static and the test above proves nothing):'
& python (Join-Path $PSScriptRoot 'imgdiff.py') $shots[0] $shots[1] |
    ForEach-Object { '  ' + $_ }
