# Sets the game's texture filtering, which is the single biggest lever against
# shimmering in VR.
#
# NOLF defaults to no anisotropic filtering. Floors and walls seen at an angle -
# which in a headset is most of what you look at - alias badly without it, and
# the shimmer is far more obvious in VR than on a monitor because your head is
# never perfectly still.
#
# Cost is negligible on modern hardware; this is a 2000-era renderer.

param([int]$Anisotropic = 16)

$ErrorActionPreference = 'Stop'
$cfg = Join-Path (Split-Path $PSScriptRoot -Parent) 'game\autoexec.cfg'
if (-not (Test-Path $cfg)) { throw "autoexec.cfg not found at $cfg" }

$text = @(Get-Content $cfg)

function SetVar([string[]]$lines, [string]$name, [string]$value) {
    $pattern = '^"' + $name + '" '
    $found = $false
    $out = foreach ($l in $lines) {
        if ($l -match $pattern) { $found = $true; "`"$name`" `"$value`"" } else { $l }
    }
    if (-not $found) { $out = @($out) + "`"$name`" `"$value`"" }
    return $out
}

$text = SetVar $text 'Anisotropic' "$Anisotropic"
$text = SetVar $text 'Trilinear'   '1'
$text = SetVar $text 'Bilinear'    '1'

Set-Content $cfg $text -Encoding ASCII

Write-Host "Texture filtering: anisotropic ${Anisotropic}x, trilinear on" -ForegroundColor Green
Get-Content $cfg | Select-String -Pattern 'Anisotropic|Trilinear|Bilinear' | ForEach-Object { "  " + $_.Line }
