# Stages or removes dgVoodoo2's DirectX 1-7 wrappers in the game folder.
#
# WHY THIS EXISTS
#
# M4 is measured: a CPU readback through real DirectDraw costs 7.1 ms per
# megapixel, about 31 ms for the game's own surface against a 0.8 ms budget, and
# 92% of it is the video-memory-to-system-memory Blt (docs/M4-READBACK-MEASURED.md).
# That kills the native D3D7 transport outright.
#
# dgVoodoo2 is the only remaining candidate that could beat window capture,
# because it does not optimise that transfer - it removes it. It reimplements
# DirectX 1-7 on top of Direct3D 11/12, so the game's surfaces become D3D11
# resources, and a D3D11 texture can be shared with the x64 host without ever
# crossing to system memory.
#
#   .\dgvoodoo.ps1 -On      stage the wrappers
#   .\dgvoodoo.ps1 -Off     remove them (the game returns to system DirectDraw)
#   .\dgvoodoo.ps1          report what is staged
#
# Reverting is deleting three files. Nothing else in game\ is touched, and the
# proxy DINPUT.dll that fixes the startup crash is left alone.

param([switch]$On, [switch]$Off)

$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root 'game'
$Src  = Join-Path $Root 'libs\dgvoodoo\extracted'

# 32-bit, because lithtech.exe is. MS\x86 holds the DirectX 1-7 wrappers;
# MS\x64 would load into nothing and look like "dgVoodoo did not work".
$Bin  = Join-Path $Src 'MS\x86'

$files = @('DDraw.dll', 'D3DImm.dll')

function Show-State {
    Write-Host ''
    Write-Host 'game\ currently holds:' -ForegroundColor Cyan
    foreach ($f in ($files + 'dgVoodoo.conf' + 'DDRAW.dll' + 'DDRAW.dll.disabled' + 'DINPUT.dll')) {
        $p = Join-Path $Game $f
        if (Test-Path $p) {
            $item = Get-Item $p
            "  {0,-22} {1,10:N0} bytes  {2}" -f $item.Name, $item.Length, $item.LastWriteTime
        }
    }
    Write-Host ''
    if (Test-Path (Join-Path $Game 'D3DImm.dll')) {
        Write-Host 'dgVoodoo2 is ACTIVE.' -ForegroundColor Green
    } else {
        Write-Host 'dgVoodoo2 is not staged - the game uses the system DirectDraw.' -ForegroundColor DarkGray
    }
}

if ($Off) {
    foreach ($f in ($files + 'dgVoodoo.conf')) {
        $p = Join-Path $Game $f
        if (Test-Path $p) { Remove-Item $p -Force; Write-Host "removed $f" -ForegroundColor Yellow }
    }
    Show-State
    return
}

if (-not $On) { Show-State; return }

# --- stage ---------------------------------------------------------------------
if (-not (Test-Path $Bin)) {
    throw "dgVoodoo2 not extracted at $Bin - unzip libs\dgvoodoo\dgVoodoo2_87_3.zip into libs\dgvoodoo\extracted first."
}

# Our own proxy DDRAW.dll must not be in the way: dgVoodoo ships its own, and
# two of them cannot both be game\DDRAW.dll.
$ours = Join-Path $Game 'DDRAW.dll'
if (Test-Path $ours) {
    $sig = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($ours))
    if ($sig.Contains('NolfVrProbeReadback')) {
        Move-Item $ours (Join-Path $Game 'DDRAW.dll.disabled') -Force
        Write-Host 'parked our own proxy DDRAW.dll (it cannot coexist with dgVoodoo)' -ForegroundColor Yellow
    }
}

foreach ($f in $files) {
    Copy-Item (Join-Path $Bin $f) (Join-Path $Game $f) -Force
    Write-Host "staged $f" -ForegroundColor DarkGray
}

# --- config --------------------------------------------------------------------
# Two settings are not optional for this project:
#
#   dgVoodooWatermark = false
#       It draws a logo into the rendered frame. That frame is not a preview -
#       it IS the image sent to the eyes, so a watermark would appear in the
#       headset and in every captured frame.
#
#   FullScreenMode = false
#       The whole capture path depends on the game being a window.
$conf = Get-Content (Join-Path $Src 'dgVoodoo.conf')
$conf = $conf -replace '^(dgVoodooWatermark\s*=\s*).*', '${1}false'
$conf = $conf -replace '^(FullScreenMode\s*=\s*).*',     '${1}false'
Set-Content (Join-Path $Game 'dgVoodoo.conf') $conf -Encoding ASCII
Write-Host 'staged dgVoodoo.conf (watermark off, windowed)' -ForegroundColor DarkGray

Show-State
Write-Host 'Run the game as usual. If it does not start, .\dgvoodoo.ps1 -Off reverts it.' -ForegroundColor Cyan
