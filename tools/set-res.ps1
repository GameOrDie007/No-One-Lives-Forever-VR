# Sets the game's render resolution directly in autoexec.cfg.
#
# The Display options menu only offers modes the renderer enumerates, but the
# engine honours whatever is in the config - that is how 3840x2160 was measured
# back at M0. Use this to pick a resolution the menu does not list.
#
# Each eye gets half the width, so pick a width that is double what you want
# per eye. The window plus its title bar must fit on screen for capture to work.

param(
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [switch]$Windowed = $true
)

# The per-eye aspect should match the headset's frustum, or coverage has to be
# bought with extra field of view and the sharpness paid for in every direction.
# Quest 3, SYMMETRIC render: tan(54)/tan(55) = 0.964. That was the shape of
# the containing frustum the client had to render because d3d.ren could only
# build symmetric ones.
#
# Quest 3, NATIVE per-eye frustum (+StubNativeFrustum 1): the shape to match
# is the eye's own frustum, and there is no containment to do -
#   (tan54 + tan40) / (tan44 + tan55) = 2.2155 / 2.3938 = 0.925
$idealEyeAspect       = 0.964
$idealEyeAspectNative = 0.925
$eyeAspect = ($Width / 2.0) / $Height
$idealWidth = [int]([math]::Round($Height * $idealEyeAspect * 2 / 2) * 2)

$ErrorActionPreference = 'Stop'
$cfg = Join-Path (Split-Path $PSScriptRoot -Parent) 'game\autoexec.cfg'
if (-not (Test-Path $cfg)) { throw "autoexec.cfg not found at $cfg" }

$text = Get-Content $cfg
$text = $text -replace '^"ScreenWidth" .*',  "`"ScreenWidth`" `"$Width`""
$text = $text -replace '^"ScreenHeight" .*', "`"ScreenHeight`" `"$Height`""
$text = $text -replace '^"Windowed" .*',     "`"Windowed`" `"$(if ($Windowed) { 1 } else { 0 })`""
Set-Content $cfg $text -Encoding ASCII

$perEye = "$([int]($Width/2))x$Height"
Write-Host "Set ${Width}x${Height} ($perEye per eye), windowed" -ForegroundColor Green
$msg = "  per-eye aspect {0:N3} - symmetric render wants {1:N3}, native per-eye frustum wants {2:N3}"
Write-Host ($msg -f $eyeAspect, $idealEyeAspect, $idealEyeAspectNative)
$idealWidthNative = [int]([math]::Round($Height * $idealEyeAspectNative * 2 / 2) * 2)
Write-Host ("  -> ${idealWidthNative}x${Height} matches the native frustum exactly")
if ([math]::Abs($eyeAspect - $idealEyeAspect) -gt 0.02) {
    Write-Host ("  -> ${idealWidth}x${Height} would match the headset exactly and waste no pixels") -ForegroundColor Yellow
}

$screenW = (Get-CimInstance Win32_VideoController |
            Where-Object { $_.CurrentHorizontalResolution } |
            Sort-Object CurrentHorizontalResolution -Descending |
            Select-Object -First 1).CurrentHorizontalResolution
# The window is borderless at (0,0), so it fits exactly at the screen width -
# the old 40px allowance was for a title bar that no longer exists and fired
# falsely on the one resolution that uses the display fully.
if ($screenW -and $Width -gt $screenW) {
    Write-Host "WARNING: ${Width} is wider than the $screenW-wide display." -ForegroundColor Yellow
    Write-Host "  Capture reads the window's compositor surface, so an oversized window MAY still work - but it is untested here." -ForegroundColor Yellow
}
